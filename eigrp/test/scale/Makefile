# Portable route-volume, fault-stress and authenticated tests.
# All human-facing suite/group names are defined by the repository Makefile.
PYTHON ?= ../../../tools/python

.PHONY: help build internal external line kite peer-routes \
	withdraw mcast linkdown mixed-tlv churn auth-md5

help:
	@printf '%s\n' 'Use make -C eigrp/test/scale <target> for an individual test:' \
	  '  internal     100K internal routes into R1' \
	  '  external     100K external routes into R1' \
	  '  line         100K mixed routes over R1-R2-R3' \
	  '  kite         100K mixed routes via three-router kite' \
	  '  peer-routes  2,048 peers each advertising 50 routes' \
	  '  withdraw     Add, withdraw and reinject 100K host routes' \
	  '  mcast        Delayed-ACK multicast pacing and recovery' \
	  '  linkdown     Fail/recover R1-R2 during route injection' \
	  '  mixed-tlv    TLV1/TLV2 peers on the same interface' \
	  '  churn        Repeated route changes, deletes and reinjection' \
	  '  auth-md5     Authenticated 100K-route redistribution' \
	  'For suites, run make scale-100k, scale-stress, scale-auth or scale-all at root.'

build:
	$(MAKE) -C ../../.. build
	$(MAKE) -C ../uut eigrp-uut-node

internal external line kite peer-routes: build
	$(PYTHON) routes_100k.py $(if $(filter $@,internal),1,$(if $(filter $@,external),2,$(if $(filter $@,line),3,$(if $(filter $@,kite),4,5))))

withdraw mcast linkdown mixed-tlv churn auth-md5: build
	$(PYTHON) stress_extended.py $@
