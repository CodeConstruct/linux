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

Requesters and responders
-------------------------

As per the OBMF-ICP spec, a channel may implement a requester, a responder,
or both. The obmf-icp core provides a facility to create a struct
obmf_icp_requester and/or a struct obmf_icp_responder for each channel, which
provides a facility to construct requests to and receive requests from
the remote OBMF-ICP endpoint

Requesters
----------

On probe, a channel driver will create a requester using
obmf_icp_channel_init_requester(). This single requester is used to initiate
all requests for the channel. The spec allows only one request to be
outstanding on each channel at a time, so only one requester may be created
for any one channel.

To perform requests to the channel device, a driver implementation should:

 - acquire the requester, using obmf_icp_channel_get_requester()

 - format the request data, using obmf_icp_format_request() (or better, one
   of the typed helpers, see below for details).

 - perform the request/response transaction using obmf_icp_requester_txn()

 - access the response data via obmf_icp_get_response()

 - release the requester, using obmf_icp_channel_put_requester().

Constructing request data & typed request helpers
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

To form a request message, drivers will call obmf_icp_format_request:

.. code-block:: c

  void *obmf_icp_format_request(struct obmf_icp_channel_device *chan,
                                struct obmf_icp_requester *req,
                                size_t payload_len);

This initialises the outgoing request data in @req, for a message pertaining to
@chan, of length @payload_len. It returns a pointer to the request payload,
for the caller to populate.

However, this is nearing dangerous-pointer-country with the void * return
buffer, so we have a couple of helpers that enforce correct typing of request
data. You should use these instead of the raw _format_request() function.

Almost all OBMF-ICP messages involve a fixed header format, followed by a
zero or more identically-structured entries.

For simple fixed-sized messages (ie., with zero entries)
obmf_icp_format_request_typed() gives us a typed version of this, enforcing
that the payload size is suitable for a structured message:

.. code-block:: c

  struct my_req {
      __le16 field_one;
      u8 field_two;
  } __packed;

  struct my_req *req = obmf_icp_format_request_typed(chan, req, struct my_req);

This enforces that the request payload has enough space for the my_req struct.

For requests that consist of a header plus a varying amount of entries, we have
obmf_icp_format_request_flex(), which handles message types that contain a flex
array:

.. code-block:: c

  struct my_req_entry {
      u8 f1;
      u8 f2;
  } __packed;

  struct my_req {
      u8 n_entries;
      struct my_req_entry entries[];
  } __packed;

  struct my_req *req = obmf_icp_format_request_flex(chan, req,
                                                    struct my_req,
                                                    entries, /* flex member */
                                                    n_entries);

This enforces that the request payload has enough space for the my_req struct,
including n_entries worth of my_req_entry.

Response data
^^^^^^^^^^^^^

Once obmf_icp_requester_txn() has completed, a driver can access the response
data through obmf_icp_get_response(). This provides access to the
response status directly, and a pointer to the response payload.

Responders
----------

Just like with the requester functionality: on probe, a channel driver will
create a responder using obmf_icp_channel_init_responder(). This responder
will invoke the passed function pointer on incoming requests.

This function pointer is responsible for interpreting the incoming request,
performing some device action, and forming the outgoing response. The
obmf-icp core will arrange for transmission of the formed response.

The callback is invoked in sleepable context. However, only one request can
be pending (per channel) at a time, so the requester will be blocked from
issuing further requests until the response has been received.

To handle requests from the channel device, a driver implementation's
responder callback should:

 - read the request data through obmf_icp_get_request()

 - validate for size and data

 - perform any request-specific logic

 - format the response data, using obmf_icp_format_response(), or a typed
   helper

Like the request helpers, there is an equivalents for fixed-size responses
in obmf_icp_format_response_typed() and for header + entries responses in
obmf_icp_format_response_flex().
