/* usbtmst4.c   -  USB device driver for timestamp test unit 4, with jtag loader

  Copyright (C) 2015, 2018, 2022 Christian Kurtsiefer <christian.kurtsiefer@gmail.com>

  This driver provides a simple interface to the control commands passed onto
  the device via the EP1 interface. Commands can be found in
  "timestampcontrol.h" for the core operation, and in "usbprog_io.h" for the
  programming section.  Commands are implemented mostly with ioctl() calls.
  
  The main data transfer takes place via a memory segment allocated via a
  mmap command; this allows efficient copying of the code to userspace without
  copying.


  status: 17.1.2015 chk  first code transfer form usbfastadc
          31.5.2018 chk  adding commands for LUT loading
	  22.6.2018 chk fixing new page fault interface for kernel >4.10.0
	  16.7.2018 chk - removing some legacy stuff, including page mgmt
	  10.5.2022 chk - added DAC code
*/


#include <linux/module.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <asm/ioctl.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/jiffies.h>  /* for irq rate servo */
#include <linux/version.h>
#include <linux/uaccess.h>

#include "timestampcontrol.h"    /* contains ioctls */
#include "usbprog_io.h"    /* contains ioctls for programming */


/* dirty fixes for broken usb_driver definitions */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,11) )
#define HAS_NO_OWNER so_sad
#define HAS_NO_DEVFS_MODE
#endif

/* fix the nopage -> fault method transition */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,22) )
#define HAS_FAULT_METHOD
#endif

/* deal with reborn page count command - FIXME: find correct definition */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(4,5,0))
#define HAS_SET_PAGE_COUNT
#endif

/* deal with newer fault code API */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(4,10,0))
#define HAS_LEAN_VM_FAULT 
#endif

/* newer kernel versions: vm_fault_t is defined */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(4,17,0))
#define HAS_VM_FAULT_TYPE
#endif

/* vm wrapper API defined, vm_flags now read-only */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(6,3,0))
#define HAS_VM_FLAG_API
#endif

/* renamed page macro */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0))
#define MAX_ORDER MAX_PAGE_ORDER
#endif

/* make vm_fault_t to int as in older kernels */
#ifndef HAS_VM_FAULT_TYPE
#define vm_fault_t int
#endif



/* to include legacy stuff, will be thrown out as soon as confirmed to work */
/* this definitely needs to be set for kernel 4.4, and has to be out for
   RASPBERRY PI devices. Others to be tested. */
#define LEGACY_PAGECOUNT


#ifdef LEGACY_PAGECOUNT
/* fix missing page count command in kernel version >2.6.13 or so */
#ifndef HAS_SET_PAGE_COUNT
static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_count, v);
}
#endif
#endif


/* Module parameters*/
MODULE_AUTHOR("Christian Kurtsiefer <christian.kurtsiefer@gmail.com>");
MODULE_DESCRIPTION("Experimental driver for timestamp test card\n");
MODULE_LICENSE("GPL");  

#define USBDEV_NAME "tmst4"   /* used everywhere... */
#define USB_VENDOR_ID_CYPRESS 0x04b4
#define USB_VENDOR_ID_S_FIFTEEN 0x3137
#define USB_DEVICE_ID 0x123a
#define USB_DEVICE_ID_S_FIFTEEN 0x200a


#define URBS_NUMBER 4 /* consecutive urbs to be allocated */



/* local status variables for cards */
typedef struct cardinfo {
    int iocard_opened;
    int major;
    int minor;
    struct usb_device *dev;
    struct device *hostdev; /* roof device */
    unsigned int outpipe1; /* contains pipe ID */ 
    unsigned int inpipe1;
    unsigned int inpipe2; /* EP2 large input pipe */
    int maxpacket; /* maximum packet size for EP2 */
    struct cardinfo *next, *previous; /* for device management */

    struct dma_page_pointer * dma_main_pointer; /* scatter list base */
    struct urb **urblist; /* pointer to urblist */
    int totalurbs; /* number of urbs allocated */
    int transfers_running; /* if !=0, new urbs can be submitted as soon as
			      they fall free */
    struct dma_page_pointer *current_free_mempiece; /* next page to be used */
    unsigned long current_free_offset; /* address within that block */
    int received_bytes;    /* number of received bytes so far */
    int errstat;           /* error status set during a callback */
    char *scratchbuf;       /* points to a DMA-capable scratch buffer for
			      small urbs */

    /* for interrupt frequency servo. This tries to arrange for a usb irq rate
       between 1 and 10 jiffies */
    unsigned int oldjiffies; /* contains last irq timestamp */
    int avgdiff;  /* low pass filter status variable for the number of jiffies
		     between calls. Is a fixed comma quantity were a value 256
		     corresponds to 1 jiffie difference per call */
    int jiffservocounter;  /* delay status variable which waits for updates
			    see also constant DEFAULT_JIFFSERVOPERIODE */
    int current_transferlength;  /* length sent to the submit_urb command */
    int initial_transferlength; /* when starting USB line */

    int smallpageorder; /* smallest page order we got in malloc */
    int minmempiece;   /* granularity of DMA buffer */

    /* for proper disconnecting behaviour */
    wait_queue_head_t closingqueue; /* for the unload to wait until closed */
    int reallygone;  /* gets set to 1 just before leaving the close call */

} cdi;


#define DEFAULT_JIFFSERVOPERIODE 5 /* update rate */

static struct cardinfo *cif=NULL; /* no device registered */

/* search cardlists for a particular minor number */
static struct cardinfo *search_cardlist(int index) {
    struct cardinfo *cp;
    for (cp=cif;cp;cp=cp->next) if (cp->minor==index) break;
    return cp; /* pointer to current private device data */
}


/*************************************************************************/
/* Memory stuff  for DMA buffer mapped into user space                   */

/* quick and dirty transient definitions 2.2 -> 2.4 ; still alive in 2.6???*/
#define VMA_OFFSET(vma)   (vma->vm_pgoff * PAGE_SIZE)

/* structure containing the scatter list for the memory. This is probably old
   stuff, and should be replaced by newer scattelists from the kernel.
   still contains stuff for easy acces for a pci busmaster, and needs new stuff
   to entertain the urbs efficiently. */
typedef struct dma_page_pointer {
    struct dma_page_pointer * next;  /* pointer to next DMA page cluster */
    struct dma_page_pointer * previous; /* pointer to previous page cluster */
    unsigned long size; /* size in bytes of current cluster */
    unsigned long fullsize; /* full size of allocated cluster, = 4k<<order */
    unsigned long order;   /*  order of mem piece */
    char * buffer; /* pointer to actual buffer (as seen by kernel) */
    dma_addr_t physicaladdress;  /* address type used for DMA */
} dma_page_pointer;



#ifdef LEGACY_PAGECOUNT
/* code to manipulate page counters of page blocks */
void  add_individual_page_counts(void *buffer, unsigned long order) {
    int i,orig;
    struct page *spa;
    if (order) {
	orig=page_count(virt_to_page(buffer)); /* first page setting */
	for (i=1;i<(1 << order);i++) {
	    spa=virt_to_page(buffer+i*PAGE_SIZE);
	    set_page_count(spa,orig);
	}
    }	  
}
void release_individual_page_counts(void *buffer, unsigned long order) {
    int i;
    struct page *spa;
    if (order) {
	for (i=1;i<(1 << order);i++) {
	    spa=virt_to_page(buffer+i*PAGE_SIZE);
	    set_page_count(spa,0);
	}
    }	  
}
#endif


/* release code for the DMA buffer and the DMA buffer pointer */
void release_dma_buffer(struct cardinfo *cp){
    struct dma_page_pointer *currbuf, *tmpbuf;
    
    /* only one buffer to free... */
    currbuf=cp->dma_main_pointer;
    if (currbuf) { /* nothing to release ?*/
	do {	    
#ifdef LEGACY_PAGECOUNT
	    /* undo page count manipulation thing?? */
	    release_individual_page_counts(currbuf->buffer, currbuf->order);
	    /* ..then give pages back to OS */
	    /* free_pages((unsigned long) currbuf->buffer,
		       currbuf->order);  free buffer */
#endif
	    /* new version with kernel tools */
	    dma_free_coherent(cp->hostdev,
			      PAGE_SIZE<<currbuf->order,
			      currbuf->buffer, currbuf->physicaladdress);
	    tmpbuf =currbuf->next; kfree(currbuf); 
	    currbuf=tmpbuf; /* free pointer */
	} while (currbuf != cp->dma_main_pointer);
	cp->dma_main_pointer=NULL; /* mark buffer empty */
    };
}
/* routine to allocate DMA buffer RAM of size (in bytes).
   Returns 0 on success, and <0 on fail */ 
static int get_dma_buffer(ssize_t size, struct cardinfo *cp) {
    ssize_t bytes_to_get = size & ~0x3; /* get long-aligned */
    ssize_t usedbytes;
    unsigned long page_order;
    void * bufferpiece;
    dma_addr_t physicaladdress;
    
    struct dma_page_pointer * currbuf;
    struct dma_page_pointer * tmpbuf;
    
    /* reset dma pointer buffer */
    currbuf=NULL; /* dma_main_pointer; */ /* NULL if no buffer exists */
    
    /* still have to get only small pieces....?? */
    page_order = 4;
    
    /* page_order = get_order(bytes_to_get); */
    if (page_order >= MAX_ORDER) page_order=MAX_ORDER;
    
    while (bytes_to_get>0) {
	/* shrink size if possible */
	while((page_order>0) && (PAGE_SIZE<<(page_order-1))>=bytes_to_get)
	    page_order--;
	
	/* transition to dma kernel tools */
	bufferpiece = dma_alloc_coherent(cp->hostdev,
					 PAGE_SIZE<<page_order,
					 &physicaladdress,
					 GFP_KERNEL);
	/* old style: */
	/* bufferpiece = (void *)__get_free_pages(GFP_KERNEL,page_order); */
	
	if (bufferpiece) {
	    
#ifdef LEGACY_PAGECOUNT
	    /* repair missing page counts */
	    add_individual_page_counts(bufferpiece, page_order);
#endif	    
	  
	    /* success: make new entry in chain */
	    tmpbuf = (dma_page_pointer *) kmalloc(sizeof(dma_page_pointer),
						  GFP_KERNEL); 
	    if (!tmpbuf) {
		printk(" Wruagh - kmalloc failed for buffer pointer....\n");
		/* give it back */
		/* old style: 
		   free_pages((unsigned long)bufferpiece,page_order);  */
		dma_free_coherent(cp->dev->bus->controller, /* get a device */
				  PAGE_SIZE<<page_order,
				  bufferpiece, physicaladdress);
		printk("kmalloc failed during DMA buffer alloc.\n");
		return -ENOMEM;
	    }
	    
	    
	    if (currbuf) { /* there is already a structure */
		/* fill new struct; currbuf points to last structure filled  */
		tmpbuf->next=currbuf->next; tmpbuf->previous=currbuf;
		/* insert in chain */
		currbuf->next->previous=tmpbuf;currbuf->next=tmpbuf;
		currbuf=tmpbuf;
	    } else {
		tmpbuf->previous=tmpbuf;
		tmpbuf->next=tmpbuf; /* fill new struct */
		currbuf=tmpbuf;
		cp->dma_main_pointer=currbuf; /* set main pointer */
	    };
	    
	    /* fill structure with actual buffer info */
	    usedbytes = PAGE_SIZE<<page_order;
	    currbuf->fullsize = usedbytes; /* all allocated bytes */
	    usedbytes = (usedbytes>bytes_to_get?bytes_to_get:usedbytes);
	    currbuf->size=usedbytes; /* get useful size into buffer */
	    currbuf->order=page_order; /* needed for free_pages */
	    currbuf->buffer=bufferpiece;  /* kernel address of buffer */
	    currbuf->physicaladdress=physicaladdress; /* used for DMA later */
	    
	    /* less work to do.. */
	    bytes_to_get -= usedbytes;
	} else {
	    /* could not get the large mem piece. try smaller ones */
	    if (page_order>0) {
		page_order--; continue;
	    } else {
		break; /* stop and clean up in case of problems */
	    };
	}
    }
    if (bytes_to_get <=0) {
	cp->smallpageorder = page_order;
	cp->minmempiece = (PAGE_SIZE << page_order);
	return 0; /* everything went fine.... */
    }
    /* cleanup of unused buffers and pointers with standard release code */
    release_dma_buffer(cp);
    return -ENOMEM;
}
/* how many bytes are transferred already ? 
   modified inconsistency: the negative indicator (MSB) in the return value
   indicates an error, but we need to avoid count rollover to cause an error
   to be detected. Therefore, true counts are given as pos numbers truncated
   to 31 bit, and a neg number means an error.
*/

static int already_transferred_bytes(struct cardinfo *cp) {
    /* still to fix: error treatment */
    if (cp->errstat) return -1;
    /* everything went fine... */
    return (cp->received_bytes & 0x7fffffff); /* do some spinlock stuff? */
}

/* completion handler for urbs; this callback should re-populate urbs as
   they fall free */
/* old code seemed to use other prototype; no idea when this went, it was
   already away in kernel version 2.6.22. regs not needed, so just drop it
   and hope I get a feedback if itbreaks something old:
   static void completion_handler(struct urb *urb, struct pt_regs *regs) { */
static void completion_handler(struct urb *urb) {
    struct cardinfo *cp=(struct cardinfo *)urb->context;
    unsigned int jf,jd; /* stores jiffies and difference */
    int tfl; /* transfer length for the next round */
    /* test about the status */
    if (urb->status) { /* something happened */
	cp->transfers_running=0;
	printk("urb accident; status: %d\n",urb->status);
	cp->errstat=urb->status;
    } else { /* urb is finished */
	/* fix current_received count and clean up mem if necessary */
	if (urb->actual_length < urb->transfer_buffer_length) {
	    /* patch up rest with zeros; this needs transfer_buffer to
	       contain the virtual address of the DMA buffer; I hope
	       that the urb handlers don't spoil this and allow me
	       to use that unused variable quietly.... */
	    memset(&((char *)urb->transfer_buffer)[urb->actual_length],
		  0, urb->transfer_buffer_length-urb->actual_length);
	}
	/* notify reader */
	cp->received_bytes += urb->transfer_buffer_length;

	/* buffer length servo to keep interrupt rate below 100 Hz */
	jf=jiffies; jd=(jf-cp->oldjiffies)*256; cp->oldjiffies = jf;
	cp->avgdiff += (((int)jd)-cp->avgdiff)/8; /* lowpass over 8 irq calls */
	/* This is some upper limit to avoid a latch-up of the servo */
	if (cp->avgdiff > 0x10000) cp->avgdiff=0x10000;
	if ((cp->jiffservocounter--)<=0) { /* now we can consider changing */
	    cp->jiffservocounter=DEFAULT_JIFFSERVOPERIODE; /* reload counter */
	    if (cp->avgdiff <256) {/* less than 1 jiffie difference */
		/* increase periode if possible */
		if (cp->current_transferlength < cp->minmempiece) {
		    cp->current_transferlength <<=1;
		    /* printk("%s: transfer len increased to %d; avgdiff: %d, jd: %d\n",
		    	   USBDEV_NAME, 
			   cp->current_transferlength,cp->avgdiff,jd); */
		}
	    }
	    if (cp->avgdiff >2500) {/* more  than 10 jiffies difference */
		/* decrease periode if possible */ 
		if (cp->current_transferlength > cp->maxpacket) {
		    cp->current_transferlength >>=1;
		    /*  printk("%s: transfer len decreased to %d; avgdiff: %d, jd:%d\n",
		    	   USBDEV_NAME,
			   cp->current_transferlength,cp->avgdiff,jd);*/
		}
	    }
	    
	}

	if (cp->transfers_running) { /* we still can submit urbs... */
	    /* submit new URB */
	    urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
	    urb->transfer_dma = cp->current_free_mempiece->physicaladdress +
		cp->current_free_offset;
	    /* for possibly zeroing afterwards */
	    urb->transfer_buffer = 
		&cp->current_free_mempiece->buffer[cp->current_free_offset];
	    /* make sure we never exceed a mem page boundary with transfer */
	    tfl=cp->current_transferlength;
	    if (cp->current_free_offset + tfl > 
		cp->current_free_mempiece->size)
		tfl=(cp->current_free_mempiece->size)-cp->current_free_offset;

	    /* hopefully this complies with the system to read larger
	       quantities for IN transfers  */
	    urb->transfer_buffer_length = tfl; 
	    usb_submit_urb(urb,GFP_ATOMIC); /* this is not irq context */
	    
	    /* get prepare next free address */
	    cp->current_free_offset += tfl;
	    if (cp->current_free_offset >= cp->current_free_mempiece->size) {
		/* we exceeded this page thing */
		cp->current_free_mempiece = cp->current_free_mempiece->next;
		cp->current_free_offset = 0;
	    }
	}
    }
}

/* initial filling of a urb queue */
static void initial_fillurbqueue(struct cardinfo *cp) {
    static int i;
    struct urb *urb;
    cp->current_free_mempiece = cp->dma_main_pointer;
    cp->current_free_offset = 0;
    /* initialize IRQ rate variables */
    cp->avgdiff=0;
    cp->jiffservocounter = DEFAULT_JIFFSERVOPERIODE;
    cp->current_transferlength = cp->initial_transferlength;
    cp->oldjiffies = jiffies;

    for (i=0;i<cp->totalurbs;i++) {
	urb=cp->urblist[i];
	/* set flags properly */
	urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;
	urb->transfer_dma = cp->current_free_mempiece->physicaladdress +
	    cp->current_free_offset;
	/* for possibly zeroing afterwards */
	urb->transfer_buffer = 
	    &cp->current_free_mempiece->buffer[cp->current_free_offset];
	/* get prepare next free address */
	cp->current_free_offset += cp->current_transferlength;
	if (cp->current_free_offset >= cp->current_free_mempiece->size) {
	    /* we exceeded this page thing */
	    cp->current_free_mempiece = cp->current_free_mempiece->next;
	    cp->current_free_offset = 0;
	}
	/* dynamic packet adjustment */
	urb->transfer_buffer_length = cp->current_transferlength;
	/* submit urb - or do we need some status cleanup? */
	usb_submit_urb(urb,GFP_KERNEL); /* this is not irq context */
    }
}

/* kill running urbs */
static void shutdown_urbs(struct cardinfo *cp){
    int i;
    cp->transfers_running=0;
    for (i=0;i<cp->totalurbs;i++) {
	usb_kill_urb(cp->urblist[i]); /* is there anything to check? */
    }

}


/*************************************************************************/
/* mmap stuff */
static void usbdev_vm_open(struct  vm_area_struct * area) {
    /* printk("vm open called.\n"); */
}
static void usbdev_vm_close(struct  vm_area_struct * area) {
    /* printk("vm close called.\n"); */
}

/*************************** transition code *************/
#ifdef HAS_FAULT_METHOD      /* new fault() code */
#ifdef HAS_LEAN_VM_FAULT
vm_fault_t usbdev_vm_fault(struct vm_fault *vmf) {
    struct cardinfo *cp = (struct cardinfo *)vmf->vma->vm_private_data;
#else
vm_fault_t usbdev_vm_fault( struct vm_area_struct *area, struct vm_fault *vmf) {
    struct cardinfo *cp = (struct cardinfo *)area->vm_private_data;
#endif
    unsigned long ofs = (vmf->pgoff << PAGE_SHIFT); /* should be addr_t ? */
    unsigned long intofs;
    unsigned char * virtad = NULL; /* start ad of page in kernel space */
    struct dma_page_pointer *pgindex;

    /* find right page */
    /* TODO: fix references... */
    pgindex = cp->dma_main_pointer; /* start with first mem piece */
    intofs=0; /* linear offset of current mempiece start */
    while (intofs+pgindex->fullsize<=ofs) {
	intofs +=pgindex->fullsize;
	pgindex=pgindex->next;
	if (pgindex == cp->dma_main_pointer) {
            /* offset is not mapped */
	    return VM_FAULT_SIGBUS; /* cannot find a proper page  */
	}
    }; /* pgindex contains now the relevant page index */
  
    /* do remap by hand */
    virtad = &pgindex->buffer[ofs-intofs];
    vmf->page = virt_to_page(virtad); /* return page index */
    get_page(vmf->page); /* increment page use counter */

    return 0;  /* everything went fine */ 
}

#else    /* here comes the old nopage code */

/* vm_operations, for the real mmap work via nopage method */
static struct page *usbdev_vm_nopage(struct vm_area_struct * area, unsigned long address, int *nopage_type) {
    struct page *page = NOPAGE_SIGBUS; /* for debug, gives mempage */
    struct cardinfo *cp = (struct cardinfo *)area->vm_private_data;
    
    /* address relative to dma memory */
    unsigned long ofs = (address - area->vm_start) + VMA_OFFSET(area);
    unsigned long intofs;
    unsigned char * virtad = NULL; /* start ad of page in kernel space */
    struct dma_page_pointer *pgindex;
    
    /* find right page */
    /* TODO: fix references... */
    pgindex = cp->dma_main_pointer; /* start with first mem piece */
    intofs=0; /* linear offset of current mempiece start */
    while (intofs+pgindex->fullsize<=ofs) {
	intofs +=pgindex->fullsize;
	pgindex=pgindex->next;
	if (pgindex == cp->dma_main_pointer) {
	    *nopage_type = VM_FAULT_SIGBUS; /* new in kernel 2.6 */
	    return NOPAGE_SIGBUS; /* ofs not mapped */
	}
    }; /* pgindex contains now the relevant page index */
    
    /* do remap by hand */
    virtad = &pgindex->buffer[ofs-intofs];  
    
    /* page table index */
    page=virt_to_page(virtad); 
    get_page(page); /* increment page use counter */
    *nopage_type = VM_FAULT_MINOR; /* new in kernel 2.6 */
    return page;
}
#endif

/* modified from kernel 2.2 to 2.4 to have only 3 entries */
static struct vm_operations_struct usbdev_vm_ops = {
    open:      usbdev_vm_open,
    close:     usbdev_vm_close,
#ifdef HAS_FAULT_METHOD
    fault:     usbdev_vm_fault,  /* introduced in kernel 2.6.23 */
#else
    nopage:    usbdev_vm_nopage, /* nopage, obsoleted in kernel 2.6.30? */
#endif
};

static int usbdev_mmap(struct file * file, struct vm_area_struct *vma) {
    struct cardinfo *cp = (struct cardinfo *)file->private_data;
    int erc; /* returned error code in case of trouble */
    int i;   /* create urb framework */
    /* try to save cp into mem private data */
    vma->vm_private_data = cp;

    /* check if memory exists already */
    if (cp->dma_main_pointer) return -EFAULT;
    
    if (VMA_OFFSET(vma)!=0) return -ENXIO; /* alignment error */
    /* should there be more checks? */
    
    /* get DMA-buffer first */
    erc=get_dma_buffer(vma->vm_end-vma->vm_start,cp); /* offset? page align? */
    if (erc) {
	printk("getmem error, code: %d\n",erc);return erc;
    }
    
    /* do mmap to user space */
    vma->vm_ops = &usbdev_vm_ops; /* get method */

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,7,0)
#ifdef HAS_VM_FLAG_API
    vm_flags_set(vma, VM_RESERVED);
#else
    vma->vm_flags |= VM_RESERVED; /* to avoid swapping ?*/
#endif
#ifdef HAS_FAULT_METHOD
#ifdef HAS_VM_FLAG_API
    vm_flags_set(vma, VM_CAN_NONLINEAR);
#else
    vma->vm_flags |= VM_CAN_NONLINEAR; /* has fault method; do we need more? */
#endif
#endif
#else
#ifdef HAS_VM_FLAG_API
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND);
#else
    vma->vm_flags |= VM_IO | VM_DONTEXPAND;/* replaces obsoleted VM_RESERVED */
#endif
#endif
    /* populate the urbs - perhaps this should go to an ioctl starting
       the engine ? */
    for (i=0;i<cp->totalurbs;i++) {
	usb_fill_bulk_urb(cp->urblist[i],cp->dev, cp->inpipe2,
			  NULL, /* transferbuffer, will be filled later */
			  2*cp->maxpacket, /* buffer length */
			  completion_handler,  /* the complete callback */
			  (void *)cp /* context pointer */ );
    }
    cp->current_free_mempiece = cp->dma_main_pointer; /* first free page */
    cp->current_free_offset = 0; /* beginning of page */
    printk("usbtmst mmap successful.\n"); /* for debugging...*/
    return 0;
}
/*************************************************************************/


/* minor device 0 (simple access) structures */
static int usbdev_flat_open(struct inode *inode, struct file *filp) {
    struct cardinfo *cp;
    struct urb *fresh_urb;
    int err; /* for error messages in opening */
    int i;
    cp= search_cardlist(iminor(inode));
    if (!cp) return -ENODEV;
    if (cp->iocard_opened) 
	return -EBUSY;
    cp->iocard_opened = 1;
    filp->private_data = (void *)cp; /* store card info in file structure */
    /* set USB device in correct alternate mode */
   
    /* look out for usb_set_interface() function */
    err=usb_set_interface(cp->dev, 0, 1); /* select alternate setting 1 */
    if (err) {
      cp->iocard_opened = 0; /* mark as closed */
      return -ENODEV; /* something happened */
    }
    /* allocate the urbs */
    cp->urblist = (struct urb **)kmalloc(sizeof(struct urb *)*URBS_NUMBER,
					 GFP_KERNEL);
    for (i=0;i<URBS_NUMBER; i++) {
	fresh_urb=usb_alloc_urb(0,GFP_KERNEL); /* may be atomic?? */
	if (!fresh_urb) { /* something bad happened; give back previous urbs */
	    while (i--) usb_free_urb(cp->urblist[i]);
	    break;
	}
	cp->urblist[i]=fresh_urb; /* save to list */
    }
    if (i<URBS_NUMBER) { /* urb allocation failed */
	printk("%s: could not allocate all %d urbs\n",USBDEV_NAME,URBS_NUMBER);
	cp->iocard_opened = 0; /* mark as closed */	
	return -ENOMEM;
    }
    cp->totalurbs=URBS_NUMBER; /* keep them */
    /* initialize transfer engine state */
    cp->transfers_running=0; /* everything is off */
    cp->errstat=0;
    cp->received_bytes=0; /* nothing transferred so far */
    cp->initial_transferlength=cp->maxpacket; /* default size */

    return 0;
}
static int usbdev_flat_close(struct inode *inode, struct file *filp) {
    struct cardinfo *cp = (struct cardinfo *)filp->private_data;
    int i; 

    /* kill eventually running urbs... */
    shutdown_urbs(cp);

    /* give back the allcoated urbs */
    for (i=0;i<cp->totalurbs;i++) usb_free_urb(cp->urblist[i]);
    cp->totalurbs=0;
    kfree(cp->urblist);

    release_dma_buffer(cp);
  
    cp->iocard_opened = 0;

    /* eventually tell the unloader that we are about to close */
    cp->reallygone=0;
    wake_up(&cp->closingqueue);
    /* don't know if this is necessary but just to make sure that we have
       really left this call */
    cp->reallygone=1;
    return 0;
}


/* -----------------------------------------------------------------*/
/* basic methods for communicating with the device */

#define MAXCHAINBYTES 1000
typedef struct bit_chain {
  int length; 
  char content[MAXCHAINBYTES];
} bitchain;

/* ioctl part that takes care of the JTAG commands */
long ioctl_jtag(unsigned int cmd, unsigned long arg, struct cardinfo *cp) {
  struct bit_chain *arg2 = (struct bit_chain *)arg;
  int i, localerr, atrf;
  char *data= cp->scratchbuf; /* DMA-compat buffer */
  int length=0; 
  int totalpayload, thispayload, headersize, dataoffset, copybytes; 
  unsigned char chksum;


  /* we need to extract data from bit stream */
  arg2=(struct bit_chain *)arg; /* interpret as pointer */
  i=copy_from_user(&length, &arg2->length, sizeof(int));
  if (i) return -ENOMEM;
  /* check if command has valid length first */
  if (length> 8000) { /* no, we have too many bits */
    return -E2BIG;
  }
  
  /* now we need to loop through a number of packets */
  totalpayload = (length+7)/8+1; /* this is in bytes, data+checksum */
  data[1] = cmd & 0xff; /* command is first byte */
  data[2] = length & 0xff;
  data[3] = (length>>8) & 0xff;
  dataoffset=0; headersize=4; chksum=0; /* values for first block */
  /* now we loop through all blocks */
  do {
    thispayload = totalpayload<64-headersize?
      totalpayload:
      64-headersize; /* may include cksm */
    data[0]=thispayload+headersize; /* how many bytes to send */
    /* calculate real bytes to copy from users */ 
    copybytes=thispayload; 
    if (totalpayload<65-headersize) copybytes--; /* we need to add chksum */
    
    if (copy_from_user(&data[headersize], 
		       &arg2->content[dataoffset], 
		       copybytes)) return -ENOMEM;
    dataoffset+=copybytes;
    
    /* calculate checksum */
    for (i=1; i<copybytes+headersize; i++) chksum += data[i];
    /* do we need to add the checksum? */
    if (totalpayload<65-headersize) data[data[0]-1]=chksum;	  
    
    /* send stuff */
    localerr=usb_bulk_msg(cp->dev, cp->outpipe1, data, data[0], &atrf, 1000);
    if (localerr) return localerr;
    totalpayload -= thispayload;
    headersize=1; /* for subsequent packages if needed */
  } while (totalpayload>0);
  
  /* retreive respond */
  totalpayload = (length+7)/8; /* this is in bytes, only data */
  dataoffset=0;
  do {
    thispayload=(totalpayload<64?totalpayload:64);
    localerr=usb_bulk_msg(cp->dev, cp->inpipe1, data,
			  64, &atrf, 1000);
    if (localerr) {
      return localerr;
    }
    if (atrf>64) return -ENOMEM;
    if (copy_to_user(&arg2->content[dataoffset],
		     data, atrf))
	return -ENOMEM;
    
    dataoffset +=atrf;
    totalpayload -= atrf;
  } while (totalpayload>0);
  
  return 0; /* everything went ok */

}

/* ioctl code that writes stuff to the SPI devices */
long ioctl_spi_write(unsigned int cmd, unsigned long arg, struct cardinfo *cp) {
  int i;
  int length, localerr, atrf;
  char *arg3= (char *) arg;
  unsigned char l3;
  unsigned char *data= cp->scratchbuf; /* DMA-compat buffer */
  
  /* these commands take pointer to a char array, which is structured as
     follows: first byte is length, followed by the commands directly */
  /* we need to extract data from bit stream */
  i=copy_from_user(&l3, &arg3[0], sizeof(char));
  if (i) return -ENOMEM;
  length=l3;
  /* check if command has valid length first */
  if (length> 58) { /* no, we have too many bytes */
    return -E2BIG;
  }
  data[0] = length+6; /* this is the length of the USB packet */ 
  data[1] = cmd & 0xff;
  data[2] = length & 0xff; /* SPI payload bytecount */
  data[3] = 0; /* not used at the moment */
  
  if (copy_from_user(&data[4], /* target position in USB packet */
		     &arg3[1], /* here starts SPI control word */ 
		     length+2)
      ) return -ENOMEM;
  
  /* send stuff */
  localerr=usb_bulk_msg(cp->dev, cp->outpipe1, data, data[0], &atrf, 1000);
  if (localerr) return localerr;
  return 0;
}

/* ioctl code that retreives stuff from the SPI devices */
long ioctl_spi_read(unsigned int cmd, unsigned long arg, struct cardinfo *cp) {
  int i;
  int length, localerr, atrf;
  char *arg3= (char *) arg;
  unsigned char l3;
  unsigned char *data= cp->scratchbuf; /* DMA-compat buffer */

  /* we need to extract data from bit stream */
  i=copy_from_user(&l3, &arg3[0], sizeof(char));
  if (i) return -ENOMEM;
  length=l3;
  /* check if command has valid length first */
  if (length> 58) { /* no, we have too many bytes */
    return -E2BIG;
  }
  data[0] = 6; /* this is the length of the OUT USB packet */ 
  data[1] = cmd & 0xff;
  data[2] = length & 0xff; /* SPI payload bytecount */
  data[3] = 0; /* not used at the moment */
  
  if (copy_from_user(&data[4], /* target position in USB packet */
		     &arg3[1], /* here starts SPI control word */ 
		     2)
      ) return -ENOMEM;
  /* send stuff */
  localerr=usb_bulk_msg(cp->dev, cp->outpipe1, data, data[0], &atrf, 1000);
  if (localerr) return localerr;
  
  
  /* retreive respond */
  localerr=usb_bulk_msg(cp->dev, cp->inpipe1, data,
			64, &atrf, 1000);
  
  if (localerr) {
    printk("usbtmst error @2; cmd: %d\n",cmd); return localerr;
  }
  if (atrf>64) return -ENOMEM;
  
  
  /* fancy return of bytecount - does that need to be so complicated? */
  l3=atrf;
  if (copy_to_user(&arg3[0], &l3, 1) ) return -ENOMEM;
 
  /* copy 'laundered' command (2 bytes) and payload data back to user */
  if (copy_to_user(&arg3[1], data, atrf) ) return -ENOMEM;
  return 0; /* everything went ok */
  
}

/* change in the ioctl structure to unlocked_ioctl...removed inode parameter */
static long usbdev_flat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct cardinfo *cp = (struct cardinfo *)filp->private_data;
    unsigned char len=3; unsigned char chksum=0;
    int atrf; /* actually transferred data */
    unsigned char *data = cp->scratchbuf; /* dma-compat bbuffer */
    int value; /* for passing stuff back */
    int localerr;
    int i;
    
    if (!cp->dev) return -ENODEV;
    
    switch (cmd) { /* to distill out real command */
    case Start_USB_machine:
	/* first check if we have the mmap done already */
	if (!cp->dma_main_pointer || cp->transfers_running) 
	    return -EBUSY; 
	/* Ok, we can start. Let's populate the urbs first....*/
	cp->transfers_running=1;
	initial_fillurbqueue(cp);
	break;
    case Stop_USB_machine:
	if (!cp->dma_main_pointer)
	    return -EBUSY; /* nothing to do or in wrong state */
	if (cp->transfers_running) /* are we down already? */ 
	    shutdown_urbs(cp); 
	break;
    case Get_transferredbytes:
	/* does not make sense if not initialized */
	if (!cp->dma_main_pointer) return -EBUSY; 
	/* otherwise get da value...modulo 0x80000000 to reserve
	   the negative sign to error conditions. ATTENTION: The
	   application must check if an overrun can take place!!!!
	*/
      return already_transferred_bytes(cp); 
      break;
    case Get_errstat:
	return cp->errstat;
	break;
    case CLOCKCHIP_WRITE: case ADCCHIP_WRITE:
	return ioctl_spi_write(cmd, arg, cp);
	break;
    case JTAG_scandata: case JTAG_scanIR:
	return ioctl_jtag(cmd, arg, cp);
	break;
    case CLOCKCHIP_READ: case ADCCHIP_READ:
	return ioctl_spi_read(cmd, arg, cp);
	break;
	/* commands that take 4 byte content from a int variable */
    case READ_RAM: case WRITE_RAM:
	i=copy_from_user(&value, (int *)arg, sizeof(int));
	if (i) return -ENOMEM;
	data[5]=(value>>24) & 0xff; len++; chksum+=data[5];
	data[4]=(value>>16) & 0xff; len++; chksum+=data[4];
	data[3]=(value>>8) & 0xff; len++; chksum+=data[4];
	data[2]=(value) & 0xff; len++; chksum+=data[4];
	goto barecommands;

	/* simple commands which send direct control urbs to the device */
    case Send_Word: /* 32 bit argument (and retreive response) */
    case START_LIMITED:
    case WRITE_CPLD_LONG: /* this is a dirty hack for only two words */
	data[5]=(arg>>24) & 0xff; len++; chksum+=data[5];
	/* three byte commands */
	//fall through
	goto threebytes;
    threebytes:
    case JTAG_forcestate:
    case JTAG_runstate:  /* second arg is a 16 bit cycle number, contained
			    in bits 23:8 of the argument */
	data[4]=(arg>>16) & 0xff; len++; chksum+=data[4];
	/* 16 bit data */
	//fall through
	goto twobytes;
    twobytes:
    case WRITE_CPLD:
    case SWD_Sendword:
    case WRITE_INPUT_DAC:
    case WRITE_EXTRA_DAC:
	data[3]=(arg>>8) & 0xff; /* MSbyte */
	len++; chksum+=data[3];
	/* one byte commands */
	//fall through
	goto onebyte;
    onebyte:
    case Set_Delay:
    case JTAG_ENDDR: /* argument is between 0 and 3, otherwise error */
    case JTAG_ENDIR: /* argument is between 0 and 3, otherwise error */
    case JTAG_setTRST: /* one byte argument, info is contained in LSB */
    case SWD_Reset:
    case SET_OVERFLOWFLAG: /* set branching in GPIF waveform */
    case SET_POWER_STATE:
    case CONFIG_TMSTDEVICE:
    case GET_TMSTCONFIG:
	data[2]=arg & 0xff; /* LSbyte or only command byte */
	if (cmd==SET_OVERFLOWFLAG) {printk("overflowdata: 0x%02x\n",data[2]);}
	len++; chksum+=data[2];
	//fall through
	goto barecommands;
	/* comands w/o argument */
    barecommands:
    case START_STREAM: case STOP_STREAM:
    case GETRDYLINESTAT:
    case Reset_Target: case Unreset_Target:
    case JTAG_initialize: case GETBYTECOUNT: 
    case FLUSH_FIFO:
    case RESET_TRANSFER: case GET_TCB:
    case GET_POWER_STATE:
    case RETREIVE_EEPROM:
    case SAVE_EEPROM:
    case PUSH_LOOKUP:
    case GET_STATUSWORD:
    case 254:  /* erase flash command */
	data[0]=len; chksum +=len;
	data[1]=cmd & 0xff; chksum +=data[1];
	data[len-1]=chksum;
	/* wait for 1 sec */
	localerr=usb_bulk_msg(cp->dev, cp->outpipe1, data, len, &atrf, 1000);
	if ((cmd != GETRDYLINESTAT) && (cmd != GETBYTECOUNT) && 
	    (cmd != GET_TCB) && (cmd != START_STREAM) &&
	    (cmd != START_LIMITED) && (cmd != GET_POWER_STATE) &&
	    (cmd != READ_RAM) && (cmd != GET_TMSTCONFIG) &&
	    (cmd != WRITE_RAM) && (cmd != RETREIVE_EEPROM) &&
	    (cmd != SAVE_EEPROM) && (cmd != GET_STATUSWORD)) {
	  return localerr; /* return error number or 0 if ok*/
	}
	break;
    default:
	return -ENOSYS; /* function not implemented */
    }

    /* now handle commands that need a response */
    if ((cmd == GETRDYLINESTAT) || (cmd == GETBYTECOUNT) 
	||(cmd == GET_TCB) || (cmd == GET_POWER_STATE)
	||(cmd == READ_RAM) || (cmd == GET_TMSTCONFIG)
	||(cmd == WRITE_RAM) || (cmd == RETREIVE_EEPROM)
	||(cmd == SAVE_EEPROM) || (cmd == GET_STATUSWORD)) {

	/* pick up some reply from the device */
	localerr=usb_bulk_msg(cp->dev, cp->inpipe1, data,
			      64, &atrf, 1000);
	if (localerr) {
	    return localerr;
	}
	if (atrf>64) return -ENOMEM;
	
	switch (cmd) {
	case GETBYTECOUNT:/* we have an int16_t return in a *int variable */
	    value=((unsigned char)data[0]) + (((unsigned char)data[1])<<8);
	    if (copy_to_user((int *)arg, &value, sizeof(int)) ) return -ENOMEM;
	    break;
	case GETRDYLINESTAT:
	    /* fancy return of bytecount int *char provided in arg */
	    if (copy_to_user((int *)arg, data, sizeof(int)) ) return -ENOMEM;
	    break;
	case GET_TCB:
        case READ_RAM:
        case GET_TMSTCONFIG:
        case GET_STATUSWORD:
	    /* copy four bytes into return value */
	    if (copy_to_user((int *)arg, data, sizeof(int)) ) return -ENOMEM;
	    break;
	case GET_POWER_STATE:
	    /* copy one bytes into return value */
	    if (copy_to_user((char *)arg, data, sizeof(char)) ) return -ENOMEM;
	    break;
	case WRITE_RAM:
	case RETREIVE_EEPROM:
	case SAVE_EEPROM:
	    /* simply return error code if return byte is !=0 */
	    if (data[0]) return -EFAULT;
	    break;
	}
	return 0;
    }
    
    /* commands that return a "try again" option. */
    if ( (cmd == START_STREAM) || 
	 (cmd == START_LIMITED) ) {
	localerr=usb_bulk_msg(cp->dev, cp->inpipe1, data, 64, &atrf, 1000);
	if (localerr) {
	    return localerr;
	}
	if (atrf>64) return -ENOMEM; /* could not retreive byte */	
	if (data[0]) {
	    return -EAGAIN; /* start did not succeed */
	}
    }
    
    return 0; /* went ok... */
}


/* minor device 0 (simple access) file options */
static struct file_operations usbdev_simple_fops = {
    open:    usbdev_flat_open,
    release: usbdev_flat_close,
    /* migration to newer ioctl definition */
    unlocked_ioctl:   usbdev_flat_ioctl,
    mmap:    usbdev_mmap,   /* port_mmap */

};


/* static structures for the class  entries for udev */
static char classname[]="usbtmst%d";


/* when using the usb major device number */
static struct usb_class_driver tmst4class = { 
    name: classname,
    fops: &usbdev_simple_fops,
#ifndef HAS_NO_DEVFS_MODE
    mode: S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP ,
#endif
    minor_base: 100, /* somewhat arbitrary choice... */
};


/* initialisation of the driver: getting resources etc. */
static int usbdev_init_one(struct usb_interface *intf, const struct usb_device_id *id ) {
    int iidx; /* index of different interfaces */
    struct usb_host_interface *setting; /* pointer to one alt setting */
    struct cardinfo *cp; /* pointer to this card */
    int found=0; /* hve found interface & setting w correct ep */
    int epi; /* end point index */
    int retval;

    /* make sure there is enough card space */
    cp = (struct cardinfo *)kmalloc(sizeof(struct cardinfo),GFP_KERNEL);
    if (!cp) {
	printk("%s: Cannot kmalloc device memory\n",USBDEV_NAME);
	return -ENOMEM;
    }
    /* get DMA-capable scratch buffer */
    cp->scratchbuf = (char *)kmalloc(256,GFP_DMA);

    cp->iocard_opened = 0; /* no open */
    cp->dma_main_pointer = NULL ; /* no DMA buffer */
    cp->totalurbs=0;   /* initially reserved urbs */
    cp->maxpacket=0; /* do we really need to initialize?? */
    cp->transfers_running = 0; /* no transfers are active */
    
    retval=usb_register_dev(intf, &tmst4class);
    if (retval) { /* coul not get minor */
	printk("%s: could not get minor for a device.\n",USBDEV_NAME);
	goto out2;
    }
    cp->minor = intf->minor;

    /* find device */
    for (iidx=0;iidx<intf->num_altsetting;iidx++){ /* probe interfaces */
	setting = &(intf->altsetting[iidx]);
	if (setting->desc.bNumEndpoints==3) {
	    for (epi=0;epi<3;epi++) {
		switch (setting->endpoint[epi].desc.bEndpointAddress) {
		    case 1:
			found |=1; break;
		    case 129: /* the  EP1 input */
			found |=2;
			break;
		    case 130: /* the large EP2 input */
			cp->maxpacket = 
			    setting->endpoint[epi].desc.wMaxPacketSize;
			found |=4;
			break;

		}
		if (found == 7) break;
	    }
	}
    }
    if (found != 7) {/* have not found correct interface */
	printk(" did not find interface; found: %d\n",found);
	goto out1; /* no device found */
    }


    /* generate usbdevice */
    cp->dev = interface_to_usbdev(intf);
    cp->hostdev = cp->dev->bus->controller; /* for nice cleanup */

    /* construct endpoint pipes */
    cp->outpipe1 = usb_sndbulkpipe(cp->dev, 1); /* construct bulk EP1 out */
    cp->inpipe1 = usb_rcvbulkpipe(cp->dev, 129); /*  EP1 in pipe */
    cp->inpipe2 = usb_rcvbulkpipe(cp->dev, 130); /* large EP2 in pipe */

    /* construct a wait queue for proper disconnect action */
    init_waitqueue_head(&cp->closingqueue);

    /* insert in list */
    cp->next=cif;cp->previous=NULL; 
    if (cif) cif->previous = cp;
    cif=cp;/* link into chain */
    usb_set_intfdata(intf, cp); /* save private data */

    return 0; /* everything is fine */
 out1:
    usb_deregister_dev(intf, &tmst4class);
 out2:
    /* first give back DMA buffer, then cardinfo structure */
    kfree(cp->scratchbuf);
    kfree(cp);
    printk("%s: dev alloc went wrong, give back %p\n",USBDEV_NAME,cp);

    return -EBUSY;
}

static void usbdev_remove_one(struct usb_interface *interface) {
    struct cardinfo *cp=NULL; /* to retreive card data */
    /* do the open race condition protection later on, perhaps with a
       semaphore */
    cp = (struct cardinfo *)usb_get_intfdata(interface);
    if (!cp) {
	printk("usbdev: Cannot find device entry \n");
	return;
    }

    /* try to find out if it is running */
    if (cp->iocard_opened) {
	printk("%s: device got unplugged while open. How messy.....\n",
	       USBDEV_NAME);
	/* we need to close things eventually */
	if (cp->transfers_running) {
	    cp->transfers_running=0;
	    /* stop DMA machinery */
	    shutdown_urbs(cp); /* is there something which does not cause
				  a call to the callback? */
	}
	/* ... and now we hope that someone realizes that we took away the
	   memory and closes the device */
	wait_event(cp->closingqueue, !(cp->iocard_opened));
	/* really don't know if this is necessary, or if wakeup comes late
	   enough */
	while (!cp->reallygone) schedule(); /* wait until it is set */
    }

    /* remove from local device list */
    if (cp->previous) { 
	cp->previous->next = cp->next;
    } else {
	cif=cp->next;
    }
    if (cp->next) cp->next->previous = cp->previous;

    /* mark interface as dead */
    usb_set_intfdata(interface, NULL);
    usb_deregister_dev(interface, &tmst4class);

    kfree(cp->scratchbuf); /* give back DMA-capable scratch buffer */
    kfree(cp); /* give back card data container structure */
    
}

/* driver description info for registration; more details?  */

static struct usb_device_id usbdev_tbl[] = {
    {USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID)},
    {USB_DEVICE(USB_VENDOR_ID_S_FIFTEEN, USB_DEVICE_ID_S_FIFTEEN)},
    {}
};

MODULE_DEVICE_TABLE(usb, usbdev_tbl);

static struct usb_driver usbdev_driver = { 
#ifndef HAS_NO_OWNER
    .owner =     THIS_MODULE,
#endif
    .name =      USBDEV_NAME, /* "usbdev-driver", */
    .id_table =  usbdev_tbl,
    .probe =     usbdev_init_one,
    .disconnect =    usbdev_remove_one,
};

static void  __exit usbdev_clean(void) {
    usb_deregister( &usbdev_driver );
}

static int __init usbdev_init(void) {
    int rc;
    cif=NULL;
    rc = usb_register( &usbdev_driver );
    if (rc) 
	pr_err("%s: usb_register failed. Err: %d",USBDEV_NAME,rc);
    return rc;
}

module_init(usbdev_init);
module_exit(usbdev_clean);
