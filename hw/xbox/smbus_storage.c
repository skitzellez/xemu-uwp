/*
 * QEMU SMBus Generic Storage Device
 *
 * Copyright (c) 2020 Mike Davis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"
#include "smbus.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "migration/vmstate.h"

#define TYPE_SMBUS_STORAGE "smbus-storage"
#define SMBUS_STORAGE(obj) OBJECT_CHECK(SMBusStorageDevice, (obj), \
    TYPE_SMBUS_STORAGE)

//#define DEBUG
#ifdef DEBUG
# define DPRINTF(format, ...) printf(format, ## __VA_ARGS__)
#else
# define DPRINTF(format, ...) do { } while (0)
#endif

typedef struct SMBusStorageDevice {
    SMBusDevice smbusdev;
    char* file;
    uint8_t *data;
    uint32_t size;
    uint8_t addr;
    uint32_t offset;
    bool persist;
} SMBusStorageDevice;

#ifdef XEMU_UWP
void uwp_log(const char *fmt, ...);
/* -1: as stored; 0 or 1: the launch link's widescreen= (ui/xemu.c), for this session only */
int xemu_uwp_widescreen = -1;

/*
 * The EEPROM's video flags (user section, offset 0x94; 0x00010000 = widescreen) set as Game Vault asked,
 * in memory only: the player's file changes only if the dashboard saves its settings. The user section
 * checksum (0x60, over 0x64..0xBF) is made again, as the kernel checks it.
 */
/* the user section checksum (0x60, over 0x64..0xBF), made again after a change: the kernel checks it */
static void uwp_eeprom_user_checksum(uint8_t *e)
{
    uint32_t high = 0, low = 0;
    for (int i = 0; i < 0x5C / 4; i++) {
        uint32_t val = ldl_le_p(e + 0x64 + 4 * i);
        uint64_t sum = ((uint64_t)high << 32) | low;
        high = (sum + val) >> 32;
        low += val;
    }
    stl_le_p(e + 0x60, ~(high + low));
}

/*
 * Games that carry several languages pick one from the EEPROM (user section, 0x90): set it to the Xbox's own
 * language for the session, in memory only (the player's file changes only if the dashboard saves). Xbox codes:
 * 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Chinese, 9 Portuguese.
 * A language the original Xbox did not have stays as stored.
 */
static void uwp_eeprom_language(uint8_t *e)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    if (!GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        return;
    }
    static const struct { const wchar_t *tag; uint32_t code; const char *what; } map[] = {
        { L"en", 1, "English" }, { L"ja", 2, "Japanese" }, { L"de", 3, "German" }, { L"fr", 4, "French" },
        { L"es", 5, "Spanish" }, { L"it", 6, "Italian" }, { L"ko", 7, "Korean" }, { L"zh", 8, "Chinese" },
        { L"pt", 9, "Portuguese" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!_wcsnicmp(name, map[i].tag, 2) && (name[2] == 0 || name[2] == L'-')) {
            uint32_t was = ldl_le_p(e + 0x90);
            if (was != map[i].code) {
                stl_le_p(e + 0x90, map[i].code);
                uwp_eeprom_user_checksum(e);
            }
            uwp_log("EEPROM: language %s for this session (the Xbox's own language; was %u)", map[i].what, was);
            return;
        }
    }
}

/*
 * The network card address (factory section, 0x40) from xemu.toml's net.mac, in memory only, with the factory
 * checksum (0x30, over 0x34..0x5F) made again. Two consoles set up from one EEPROM have the same address and
 * clash online (Insignia, system link) in one home; each can be given its own this way.
 */
static void uwp_eeprom_mac(uint8_t *e)
{
    extern const char *xemu_uwp_config_mac(void); /* ui/xemu.c */
    const char *text = xemu_uwp_config_mac();
    unsigned int b[6];
    if (!text || !text[0]) {
        return;
    }
    if (sscanf(text, "%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x", &b[0], &b[1], &b[2], &b[3], &b[4],
               &b[5]) != 6) {
        uwp_log("EEPROM: net.mac '%s' in xemu.toml is not an address like 00:50:f2:00:00:02; not used", text);
        return;
    }
    for (int i = 0; i < 6; i++) {
        e[0x40 + i] = (uint8_t)b[i];
    }
    uint32_t high = 0, low = 0;
    for (int i = 0; i < 0x2C / 4; i++) {
        uint32_t val = ldl_le_p(e + 0x34 + 4 * i);
        uint64_t sum = ((uint64_t)high << 32) | low;
        high = (sum + val) >> 32;
        low += val;
    }
    stl_le_p(e + 0x30, ~(high + low));
    uwp_log("EEPROM: network address %02x:%02x:%02x:%02x:%02x:%02x from xemu.toml for this session", b[0], b[1],
            b[2], b[3], b[4], b[5]);
}

static void uwp_eeprom_widescreen(uint8_t *e)
{
    if (xemu_uwp_widescreen < 0) {
        return;
    }
    uint32_t flags = ldl_le_p(e + 0x94);
    uint32_t want = xemu_uwp_widescreen ? flags | 0x00010000 : flags & ~0x00010000u;
    if (want != flags) {
        stl_le_p(e + 0x94, want);
        uwp_eeprom_user_checksum(e);
    }
    uwp_log("EEPROM: widescreen %s for this session", xemu_uwp_widescreen ? "on" : "off");
}
#endif

static void smbus_storage_realize(DeviceState *dev, Error **errp)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);
    qdev_prop_set_uint8(dev, "address", s->addr);
    s->data = g_malloc0(s->size);
    s->offset = 0;

    if (s->file) {
        int size = get_image_size(s->file, NULL);
        if (size != s->size) {
            error_setg(errp, "%s: file '%s' size of %d, expected %d\n",
                __func__, s->file, size, s->size);
            return;
        }

        int fd = qemu_open(s->file, O_RDONLY | O_BINARY, NULL);
        if (fd < 0) {
            error_setg(errp, "%s: file '%s' could not be opened\n",
                __func__, s->file);
            return;
        }

        int rc = read(fd, s->data, s->size);
        if (rc != s->size) {
            error_setg(errp, "%s: file '%s' read failure\n", __func__, s->file);
            close(fd);
            return;
        }
        close(fd);
#ifdef XEMU_UWP
        if (s->addr == 0x54 && s->size == 256) {
            uwp_eeprom_language(s->data);
            uwp_eeprom_widescreen(s->data);
            uwp_eeprom_mac(s->data);
        }
#endif
    } else {
        error_setg(errp, "%s: file unspecified\n", __func__);
    }
}

static int smbus_storage_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    DPRINTF("%s: addr=0x%02x cmd=0x%02x val=0x%02x\n",
        __func__, s->addr, buf[0], buf[1]);

    /* len is guaranteed to be > 0 */
    s->offset = buf[0];
    buf++;
    len--;

    bool changed = false;
    for (; len > 0; len--) {
        changed = true;
        s->data[s->offset] = *buf++;
        DPRINTF("%s: addr=0x%02x off=0x%02x, data=0x%02x\n",
            __func__, s->addr, s->offset, s->data[s->offset]);
        s->offset = (s->offset + 1) % s->size;
    }

    if (changed && s->file && s->persist) {
        int fd = qemu_open(s->file, O_WRONLY | O_BINARY, NULL);
        if (fd < 0) {
            DPRINTF("%s: file '%s' could not be opened\n", __func__, s->file);
            return -1;
        }

        int wc = write(fd, s->data, s->size);
        if (wc != s->size) {
            DPRINTF( "%s: file '%s' write failure\n", __func__, s->file);
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

static uint8_t smbus_storage_receive_byte(SMBusDevice *dev)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    uint8_t val = s->data[s->offset];
    DPRINTF("%s: addr=0x%02x off=0x%02x val=0x%02x\n",
        __func__, s->addr, s->offset, val);
    s->offset = (s->offset + 1) % s->size;

    return val;
}

static const VMStateDescription vmstate_smbus_storage = {
    .name = "smbus-storage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(smbusdev, SMBusStorageDevice),
        VMSTATE_VBUFFER_UINT32(data, SMBusStorageDevice, 1, NULL, size),
        VMSTATE_UINT32(offset, SMBusStorageDevice),
        VMSTATE_END_OF_LIST()
    }
};

// default xbox eeprom with persistence
static const Property smbus_storage_props[] = {
    DEFINE_PROP_UINT8("addr", SMBusStorageDevice, addr, 0x54),
    DEFINE_PROP_UINT32("size", SMBusStorageDevice, size, 256),
    DEFINE_PROP_BOOL("persist", SMBusStorageDevice, persist, true),
    DEFINE_PROP_STRING("file", SMBusStorageDevice, file),
};

static void smbus_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *sc = SMBUS_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_smbus_storage;
    dc->realize = smbus_storage_realize;
    sc->receive_byte = smbus_storage_receive_byte;
    sc->write_data = smbus_storage_write_data;
    device_class_set_props(dc, smbus_storage_props);
}

static TypeInfo smbus_storage_info = {
    .name = TYPE_SMBUS_STORAGE,
    .parent = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SMBusStorageDevice),
    .class_init = smbus_storage_class_init,
};

static void smbus_storage_register_devices(void)
{
    type_register_static(&smbus_storage_info);
}

type_init(smbus_storage_register_devices)
