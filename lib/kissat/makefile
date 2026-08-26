all:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build"
kissat:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" kissat
tissat:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" tissat
clean:
	rm -f "/home/huzhang/VeLoR/velor/lib/kissat"/makefile
	rm -f "/home/huzhang/VeLoR/velor/lib/kissat"/src/makefile
	-$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" clean
	rm -rf "/home/huzhang/VeLoR/velor/lib/kissat/build"
coverage:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" coverage
format:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" format
test:
	$(MAKE) -C "/home/huzhang/VeLoR/velor/lib/kissat/build" test
.PHONY: all clean coverage format kissat test tissat
