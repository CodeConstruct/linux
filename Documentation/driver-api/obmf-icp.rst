.. SPDX-License-Identifier: GPL-2.0

============================================================================
Open Boot Management Framework (OBMF) Interface Consolidation Protocol (ICP)
============================================================================

The OBMF-ICP driver framework provides three things:

 - the obmf-icp core
 - drivers for each type of "transport" for obmf-icp messaging
 - drivers for each type of obmf-icp channel device

The core provides the bus type, base device- and driver definitions, and
the channel discovery mechanism.

Transport drivers provide the underlying facility for communication with
remote OBMF-ICP devices. There is currently one transport defined: a USB
host-side transport.

Channel drivers implement channel-device specific functions, over each
devices OBMF-ICP channel communications. An OBMF-ICP device may expose many
channels, of any number of channel types (for example GPIO, serial, or
IPMI).

Transport driver API
====================

Transport drivers provide the struct obmf_icp device (ie., the top-level bus
data), along with a struct obmf_icp_transport, which provides implementations
for transport messaging operations.

Transport driver implementations should initialise hardware as needed,
allocate a struct obmf_icp, set up a struct obmf_icp_transport, and register it
via obmf_icp_bus_register(). On hardware-triggered disconnect, the bus can
be unregistered via obmf_icp_bus_unregister().

Channel driver API
==================

Drivers for OBMF-ICP channels provide a bridge between the OBMF-ICP device
(bound to a specific individual channel), and a Linux subsystem device (say,
a GPIO controller).

Channel drivers implement a struct obmf_icp_channel_driver, and (un)register
with the core through obmf_icp_channel_driver_register() and
obmf_icp_channel_driver_unregister().

Drivers are matched to device channels on the OBMF-ICP-specified channel type
value; see the OBMF_ICP_DEV_ID_TYPE macro for initialising a suitable id_table
for the driver.
