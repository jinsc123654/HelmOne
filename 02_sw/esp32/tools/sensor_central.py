#!/usr/bin/env python3
"""Reference BLE sensor central on the bench host (BlueZ over D-Bus).

It plays the role the Helm One bike computer plays: find a sensor, connect,
subscribe to its measurement characteristic, and decode the notification stream.
The decode follows the same fields the Helm One firmware reads, so a value shown
here is a value the bike computer can read.

Usage:
    python3 sensor_central.py --kind csc                # 15 s of cadence
    python3 sensor_central.py --kind cps -s 20          # power
    python3 sensor_central.py --kind hr  -s 20          # heart rate
    python3 sensor_central.py --kind cps --cp-test      # also poke the control point
"""
import argparse
import sys
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
OM_IFACE = "org.freedesktop.DBus.ObjectManager"
PROP_IFACE = "org.freedesktop.DBus.Properties"
DEV_IFACE = "org.bluez.Device1"
ADAPTER_IFACE = "org.bluez.Adapter1"
CHR_IFACE = "org.bluez.GattCharacteristic1"

UUID_BAS_LEVEL = "00002a19-0000-1000-8000-00805f9b34fb"

KINDS = {
    # kind : service uuid, measurement uuid, address, feature uuid, location uuid
    "hr":  ("0000180d-0000-1000-8000-00805f9b34fb",
            "00002a37-0000-1000-8000-00805f9b34fb",
            "C0:DE:CA:DE:00:03", None,
            "00002a38-0000-1000-8000-00805f9b34fb"),
    "csc": ("00001816-0000-1000-8000-00805f9b34fb",
            "00002a5b-0000-1000-8000-00805f9b34fb",
            "C0:DE:CA:DE:00:01",
            "00002a5c-0000-1000-8000-00805f9b34fb",
            "00002a5d-0000-1000-8000-00805f9b34fb"),
    "cps": ("00001818-0000-1000-8000-00805f9b34fb",
            "00002a63-0000-1000-8000-00805f9b34fb",
            "C0:DE:CA:DE:00:02",
            "00002a65-0000-1000-8000-00805f9b34fb",
            "00002a5d-0000-1000-8000-00805f9b34fb"),
}


def objects(bus):
    return bus.call_blocking(BLUEZ, "/", OM_IFACE, "GetManagedObjects", "", [])


def find_adapter(objs):
    for path, ifaces in objs.items():
        if ADAPTER_IFACE in ifaces:
            return path
    raise SystemExit("no bluetooth adapter on this host")


def find_device(objs, addr):
    for path, ifaces in objs.items():
        if DEV_IFACE in ifaces and str(ifaces[DEV_IFACE].get("Address", "")).upper() == addr.upper():
            return path
    return None


def prop(bus, path, iface, name):
    return bus.call_blocking(BLUEZ, path, PROP_IFACE, "Get", "", [iface, name])


def wait_for_device(bus, adapter, addr, timeout=15.0, force_scan=False):
    dev = None if force_scan else find_device(objects(bus), addr)
    if dev is not None:
        return dev

    print("scanning for %s ..." % addr)
    bus.call_blocking(BLUEZ, adapter, ADAPTER_IFACE, "StartDiscovery", "", [])
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            time.sleep(0.5)
            dev = find_device(objects(bus), addr)
            if dev is not None:
                return dev
    finally:
        try:
            bus.call_blocking(BLUEZ, adapter, ADAPTER_IFACE, "StopDiscovery", "", [])
        except dbus.exceptions.DBusException:
            pass
    raise SystemExit("device %s not found" % addr)


def connect_device(bus, adapter, addr):
    """Connect and return the device path once the link is up.

    Deliberately does not wait for ServicesResolved: bluetoothd on this host is
    slow and erratic about publishing it, while the GATT objects show up
    regardless - so discovery is left to wait_for_gatt() below."""
    dev_path = wait_for_device(bus, adapter, addr)
    dev = bus.get_object(BLUEZ, dev_path)
    dbus.Interface(dev, PROP_IFACE).Set(DEV_IFACE, "Trusted", dbus.Boolean(True))

    for attempt in (1, 2):
        try:
            dbus.Interface(dev, DEV_IFACE).Connect()
            break
        except dbus.exceptions.DBusException as exc:
            # A cached device connects to nothing if the sensor rebooted since it
            # was last seen: drop the stale entry by discovering it again.
            print("connect attempt %d failed: %s" % (attempt, exc.get_dbus_message()))
            if attempt == 2:
                raise SystemExit("could not connect to %s" % addr)
            wait_for_device(bus, adapter, addr, timeout=12.0, force_scan=True)
            time.sleep(1.0)

    started = time.time()
    deadline = started + 20.0
    while time.time() < deadline:
        dev_path = find_device(objects(bus), addr)
        if dev_path is not None:
            try:
                if bool(prop(bus, dev_path, DEV_IFACE, "Connected")):
                    return dev_path
            except dbus.exceptions.DBusException:
                pass
        time.sleep(0.2)

    raise SystemExit("link to %s did not come up" % addr)


def wait_for_gatt(bus, addr, want_uuid, timeout=75.0):
    """Poll the D-Bus tree until the wanted characteristic shows up.

    Returns (services_by_uuid, chars_by_uuid) where each value is
    (object_path, properties)."""
    started = time.time()
    deadline = started + timeout
    seen = 0

    while time.time() < deadline:
        objs = objects(bus)
        dev_path = find_device(objs, addr)
        services, chars = {}, {}
        if dev_path is not None:
            for path, ifaces in objs.items():
                if not path.startswith(dev_path + "/"):
                    continue
                if "org.bluez.GattService1" in ifaces:
                    services[str(ifaces["org.bluez.GattService1"].get("UUID", "")).lower()] = path
                if CHR_IFACE in ifaces:
                    chars[str(ifaces[CHR_IFACE].get("UUID", "")).lower()] = (path, ifaces[CHR_IFACE])

        if len(chars) != seen:
            print("  +%.1fs gatt: %d services, %d characteristics" %
                  (time.time() - started, len(services), len(chars)))
            seen = len(chars)

        if want_uuid in chars:
            return services, chars

        time.sleep(0.5)

    raise SystemExit("GATT database was not published in time")


# --- measurement decoding, mirroring the Helm One parsers ---------------

def decode_csc(value):
    if len(value) < 5:
        return None, "short packet %s" % value.hex()
    flags = value[0]
    off = 1
    wheel = None
    if flags & 0x01:
        wheel = (int.from_bytes(value[1:5], "little"), int.from_bytes(value[5:7], "little"))
        off += 6
    if not flags & 0x02:
        return None, "flags=0x%02x wheel=%s (no crank data)" % (flags, wheel)
    revs = int.from_bytes(value[off:off + 2], "little")
    evt = int.from_bytes(value[off + 2:off + 4], "little")
    return (revs, evt), "flags=0x%02x crank_revs=%-5d evt=%-5d" % (flags, revs, evt)


def decode_cps(value):
    if len(value) < 4:
        return None, "short packet %s" % value.hex()
    flags = int.from_bytes(value[0:2], "little")
    watts = int.from_bytes(value[2:4], "little", signed=True)
    text = "flags=0x%04x power=%d W" % (flags, watts)
    if flags & 0x0020 and len(value) >= 8:
        revs = int.from_bytes(value[4:6], "little")
        evt = int.from_bytes(value[6:8], "little")
        text += " crank_revs=%-5d evt=%-5d" % (revs, evt)
    return watts, text


def decode_hr(value):
    if len(value) < 2:
        return None, "short packet %s" % value.hex()
    flags = value[0]
    if flags & 0x01:
        if len(value) < 3:
            return None, "short packet %s" % value.hex()
        bpm = int.from_bytes(value[1:3], "little")
        off = 3
    else:
        bpm = value[1]
        off = 2
    contact = "n/a"
    if flags & 0x02:
        contact = "yes" if flags & 0x04 else "no"
    text = "flags=0x%02x bpm=%-3d contact=%s" % (flags, bpm, contact)
    if flags & 0x08 and off + 2 <= len(value):
        text += " energy=%d kJ" % int.from_bytes(value[off:off + 2], "little")
    return bpm, text


DECODERS = {"csc": decode_csc, "cps": decode_cps, "hr": decode_hr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", "--kind", choices=sorted(KINDS), default="csc")
    ap.add_argument("-a", "--addr", default=None)
    ap.add_argument("-s", "--seconds", type=float, default=15.0)
    ap.add_argument("--cp-test", action="store_true",
                    help="write the control point to check its error handling")
    ap.add_argument("--keep", action="store_true", help="stay connected at the end")
    args = ap.parse_args()

    svc_uuid, meas_uuid, def_addr, feat_uuid, loc_uuid = KINDS[args.kind]
    addr = args.addr or def_addr
    decode = DECODERS[args.kind]

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    adapter = find_adapter(objects(bus))
    print("connect %s (%s) ..." % (addr, args.kind))
    dev_path = connect_device(bus, adapter, addr)
    print("device path %s" % dev_path)

    print("waiting for the GATT database ...")
    services, chars = wait_for_gatt(bus, addr, meas_uuid)
    if svc_uuid not in services:
        raise SystemExit("expected service %s is missing" % svc_uuid)

    for uuid, label in ((feat_uuid, "feature"), (loc_uuid, "location"),
                        (UUID_BAS_LEVEL, "battery")):
        if not uuid or uuid not in chars:
            continue
        try:
            value = bytes(bus.call_blocking(BLUEZ, chars[uuid][0], CHR_IFACE, "ReadValue", "", [{}]))
        except dbus.exceptions.DBusException as exc:
            print("%-9s: read failed: %s" % (label, exc.get_dbus_name()))
            continue
        if label == "feature":
            print("%-9s: 0x%s" % (label, value[::-1].hex()))
        else:
            print("%-9s: %s" % (label, value.hex()))

    meas_path, meas_props = chars[meas_uuid]
    print("measurement: %s props=%s" % (meas_path, ",".join(meas_props.get("Flags", []))))

    cp_uuid = "00002a55-0000-1000-8000-00805f9b34fb"
    if args.cp_test and cp_uuid in chars:
        # No indication subscription: the profile says the sensor must answer
        # with "CCC improperly configured" (0x81).
        try:
            bus.call_blocking(BLUEZ, chars[cp_uuid][0], CHR_IFACE, "WriteValue", "",
                              [dbus.Array([dbus.Byte(0x01), dbus.Byte(0)], signature="y"), {}])
            print("control point: write accepted (expected an error)")
        except dbus.exceptions.DBusException as exc:
            print("control point: write rejected: %s" % exc.get_dbus_message().split(":", 1)[-1].strip())

    state = {"prev": None, "packets": 0, "values": []}

    def on_props(iface, changed, _invalidated, path=None):
        if iface != CHR_IFACE or path != meas_path or "Value" not in changed:
            return
        value = bytes(changed["Value"])
        state["packets"] += 1
        decoded, text = decode(value)
        extra = ""
        if decoded is not None and state["prev"] is not None:
            if args.kind == "csc":
                drev = (decoded[0] - state["prev"][0]) & 0xffff
                dtime = (decoded[1] - state["prev"][1]) & 0xffff
                if drev and dtime:
                    rpm = drev * 1024 * 60 / dtime
                    state["values"].append(rpm)
                    extra = "  -> %.1f rpm" % rpm
            elif args.kind == "cps":
                state["values"].append(decoded)
            else:
                state["values"].append(decoded)
        state["prev"] = decoded
        print("#%-3d %s%s" % (state["packets"], text, extra))

    bus.add_signal_receiver(on_props, signal_name="PropertiesChanged",
                            dbus_interface=PROP_IFACE, path_keyword="path")

    print("subscribing to notifications for %.0f s ..." % args.seconds)
    try:
        bus.call_blocking(BLUEZ, meas_path, CHR_IFACE, "StartNotify", "", [])

        loop = GLib.MainLoop()
        GLib.timeout_add(int(args.seconds * 1000), lambda: (loop.quit(), False)[1])
        try:
            loop.run()
        except KeyboardInterrupt:
            pass

        try:
            bus.call_blocking(BLUEZ, meas_path, CHR_IFACE, "StopNotify", "", [])
        except dbus.exceptions.DBusException:
            pass
    finally:
        if not args.keep:
            # Always drop the link: a sensor stops advertising while linked, so a
            # link left open makes it invisible to everything else.
            path = find_device(objects(bus), addr) or dev_path
            try:
                bus.call_blocking(BLUEZ, path, DEV_IFACE, "Disconnect", "", [])
            except dbus.exceptions.DBusException as exc:
                print("disconnect failed: %s" % exc.get_dbus_message())

    values = state["values"]
    unit = {"csc": "rpm", "cps": "W", "hr": "bpm"}[args.kind]
    print("\n%d notifications, %d %s samples" % (state["packets"], len(values), unit))
    if values:
        print("%s: min %.1f  max %.1f  mean %.1f" %
              (unit, min(values), max(values), sum(values) / len(values)))

    return 0 if values else 1


if __name__ == "__main__":
    sys.exit(main())
