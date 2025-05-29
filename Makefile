all:
	@true

install:
	pre-commit install
	pre-commit install --hook-type commit-msg

readevents7:
	[ -f /usr/bin/readevents7 ] || { make -C src/inst_efficiency/lib/usbtmst4/apps readevents7 && sudo cp -p src/inst_efficiency/lib/usbtmst4/apps/readevents7 /usr/bin/readevents7; }

usbtmst4: readevents7
	make -C src/inst_efficiency/lib/usbtmst4
