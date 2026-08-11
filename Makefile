all: driver_module user_tests

driver_module:
	$(MAKE) -C driver/

user_tests:
	$(MAKE) -C tests/

clean:
	$(MAKE) -C driver/ clean
	$(MAKE) -C tests/ clean
