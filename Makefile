all:
	@true

install:
	pre-commit install
	pre-commit install --hook-type commit-msg
