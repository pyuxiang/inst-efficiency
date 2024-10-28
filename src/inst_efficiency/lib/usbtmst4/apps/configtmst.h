/* configtmst.h: FPGA configuration register for the timestamp test unit,
   version fpga2 (first timestamp flow code). The definitions here are
   written into the FPGA with the WRITE_CPLD ioctl, with an OR-ed combination
   of values.

new config reg layout (as of svn-28):

bit 15:    InputPolarity. if 1, use positive polarity and DACVAL1 register, 
           if 0, use negative polarity and DACVAL0 register (for NIM, default)

bit 14:10  Delay
           Sets the delay value for the coincidence sampling

bit 9:     ADC_SPI_Select. When set, data writes get serialized to configure
           the ADC converter via SPI. SPI parameters get transmitted after a
           32bit data word is written. Overrides ParameterSelect bit.

bit 8      TimestampDebug
           1: longest time word is filled with debuginfo

bit 7      ParameterSelect
           1:parameter registers are written, 0: lookup table is filled

bit 6      NIMOUTenable
           When set, the output generates pulses (or whatever else)

bit 5      DummyInject
           When set, dummy events are generated to permit a rollover correction

bit 4      Shortresult
           When set, each event creates a 32 bit word. Otherwise, a 64 bit
	   word is generated (compatible with old standard?)

bit 3      FIFOreset
           when set, the FIFOs in the FPGA is reset

bit 2      CounterReset

bit 1      CollectEN
           when set, timestamp data is allowed to be written to FIFOs

bit 0      LED B indicating active acquisition



*/

#define CollectLED      (0x01)
#define CollectEN       (0x02)
#define CounterReset    (0x04)
#define FIFOreset       (0x08)

#define LongFormat      (0x00)
#define ShortFormat     (0x10)

#define DummyInject     (0x20)
#define NoDummyInject   (0x00)

#define NIMOUTenable    (0x40)
#define ParameterSelect (0x80)
#define LookuptabSelect (0x00)

#define SampleDelayShift (10)

#define TimestampDebug   (0x100)
#define ADC_SPI_Select   (0x200)
#define PositiveInputPolarity    (0x8000)
#define NegativeInputPolarity    (0)

/* for Nim parameter register at parameter register address 1 */
#define NIMdivider256   (0x00)
#define NIMdivider1024  (0x01)
#define NIMdivider16k   (0x02)
#define NIMdivider64k   (0x03)

/* for ADC preprocessing register at parameter register address 0 */
#define ADC_HiresRouting (0x8000)

