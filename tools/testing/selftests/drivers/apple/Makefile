# SPDX-License-Identifier: GPL-2.0
# macsmc-j713.py requires physical interaction and changes LED brightness.
# atc-t8132-usb3.py runs on the build host against a CPU MMIO model.
# pmgr-inherited.py checks admission and lifecycle against host register mocks.
# dwc3-initial-role.py checks initial host-role admission before core reset.
# sn201202x-transport.py checks the production SPMI selection and payload path.
TEST_PROGS := macsmc-j713.py atc-t8132-usb3.py pmgr-inherited.py dwc3-initial-role.py \
	      sn201202x-transport.py
TEST_FILES := j813-peripherals.sh

include ../../lib.mk
