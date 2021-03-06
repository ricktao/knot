#!/usr/bin/env python3

'''Test for the queryacl module'''

from dnstest.utils import *
from dnstest.test import Test
from dnstest.module import ModQueryacl

import random

t = Test(address=4)

knot = t.server("knot")
zones = t.zone_rnd(3)
t.link(zones, knot)

knot.addr_extra = ["127.0.0.2", "127.0.0.3"]

knot.add_module(zones[0], ModQueryacl(address=["127.0.0.1/32", "127.0.0.2/32"]))
knot.add_module(zones[1], ModQueryacl(interface=["127.0.0.1/32", "127.0.0.2/32"]))
knot.add_module(zones[2], ModQueryacl(address=["127.0.0.1/32", "127.0.0.2/32"],
                                      interface=["127.0.0.1/32", "127.0.0.2/32"]))

t.start()

knot.zones_wait(zones)

# Test just address ACL.
resp = knot.dig(zones[0].name, "SOA", addr="127.0.0.3", source="127.0.0.3")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[0].name, "SOA", addr="127.0.0.3", source="127.0.0.2")
resp.check(rcode="NOERROR")
resp = knot.dig(zones[0].name, "SOA", addr="127.0.0.2", source="127.0.0.3")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[0].name, "SOA", addr="127.0.0.2", source="127.0.0.2")
resp.check(rcode="NOERROR")

# Test just interface ACL.
resp = knot.dig(zones[1].name, "SOA", addr="127.0.0.3", source="127.0.0.3")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[1].name, "SOA", addr="127.0.0.3", source="127.0.0.2")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[1].name, "SOA", addr="127.0.0.2", source="127.0.0.3")
resp.check(rcode="NOERROR")
resp = knot.dig(zones[1].name, "SOA", addr="127.0.0.2", source="127.0.0.2")
resp.check(rcode="NOERROR")

# Test both address and interface ACL.
resp = knot.dig(zones[2].name, "SOA", addr="127.0.0.3", source="127.0.0.3")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[2].name, "SOA", addr="127.0.0.3", source="127.0.0.2")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[2].name, "SOA", addr="127.0.0.2", source="127.0.0.3")
resp.check(rcode="NOTAUTH")
resp = knot.dig(zones[2].name, "SOA", addr="127.0.0.2", source="127.0.0.2")
resp.check(rcode="NOERROR")

t.end()
