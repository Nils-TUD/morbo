/* -*- Mode: C -*- */

/* Based on fulda. Original copyright: */
/*
 * Copyright (C) 2007  Bernhard Kauer <kauer@tudos.org>
 * Technische Universitaet Dresden, Operating Systems Research Group
 *
 * This file is part of the FULDA package, which is distributed under
 * the  terms  of the  GNU General Public Licence 2.  Please see the
 * COPYING file for details.
 */


#include <stdbool.h>

#include <mbi.h>
#include <mbi-tools.h>
#include <morbo.h>

#include <util.h>
#include <ohci.h>
#include <ohci-registers.h>
#include <ohci-crm.h>
#include <crc16.h>
#include <asm.h>

/* Constants */

#define RESET_TIMEOUT 10000
#define PHY_TIMEOUT   10000
#define MISC_TIMEOUT  10000

/* Globals */

/* Some debugging macros */

#define OHCI_INFO(msg, args...) printf("OHCI: " msg, ## args)

/* Access to OHCI registers */
#define OHCI_REG(dev, reg) ((dev)->ohci_regs[(reg)/4])


#define DIR_TYPE_IMM    0
#define DIR_TYPE_CSROFF 1
#define DIR_TYPE_LEAF   2
#define DIR_TYPE_DIR    3

#define CROM_DIR_ENTRY(type, key_id, value) (((type) << 30) | ((key_id) << 24) | (value))

/* TODO This could be made nicer, but C sucks... */
static void
ohci_generate_crom(struct ohci_controller *ohci, enum link_speed speed)
{
  ohci_config_rom_t *crom = ohci->crom;

  /* Initialize with zero */
  memset(crom, 0, sizeof(ohci_config_rom_t));

  /* Copy the first quadlets */
  crom->field[1] = OHCI_REG(ohci, BusID);

  /* We dont want to be bus master. */
  crom->field[2] = OHCI_REG(ohci, BusOptions) & 0x0FFFFFFF;
  if (speed != SPEED_MAX) {
    if (speed > (crom->field[2] & 0xf)) {
      OHCI_INFO("Tried to set invalid speed. Ignored.\n");
    } else {
      crom->field[2] =  (crom->field[2] & ~0xf) | speed;
    }
  }
  OHCI_INFO("BusOptions set to %x.\n", crom->field[2]);

  crom->field[3] = OHCI_REG(ohci, GUIDHi);
  crom->field[4] = OHCI_REG(ohci, GUIDLo);

  /* Protect 4 words by CRC. */
  crom->field[0] = 0x04040000 | crc16(&(crom->field[1]), 4);

  /* Now we can generate a root directory */

  crom->field[5] = 4 << 16;   /* 4 words follow. Put CRC here later. */
  crom->field[6] = 0x03 << 24 | MORBO_VENDOR_ID; /* Immediate */
  crom->field[7] = 0x17 << 24 | MORBO_MODEL_ID;	 /* Immediate */
  crom->field[8] = 0x81 << 24 | 2;		 /* Text descriptor */
  crom->field[9] = MORBO_INFO_DIR << 24 | 8;	 /* Leaf */
  crom->field[5] |= crc16(&(crom->field[6]), 4);

  crom->field[10] = 0x0006 << 16; /* 6 words follow */
  crom->field[11] = 0;
  crom->field[12] = 0;
  crom->field[13] = 'Morb';
  crom->field[14] = 'o - ';
  crom->field[15] = 'OHCI';
  crom->field[16] = ' v2\0';
  crom->field[10] |= crc16(&(crom->field[11]), 6);

  crom->field[17] = 0x001 << 16; /* 1 words follow */
  crom->field[18] = (uint32_t)multiboot_info; /* Pointer to multiboot info */
  crom->field[17] |= crc16(&(crom->field[18]), 1);

}

static void
ohci_load_crom(struct ohci_controller *ohci)
{
  ohci_config_rom_t *crom = ohci->crom;
  assert(((uint32_t)crom & 1023) == 0, "Misaligned Config ROM");

  if ((OHCI_REG(ohci, HCControlSet) & HCControl_linkEnable) == 0) {
    /* We are not up yet, so set these manually. */
    OHCI_REG(ohci, ConfigROMhdr) = crom->field[0];
    OHCI_REG(ohci, BusOptions)   = crom->field[2];
  }

  /* Words must be in network byte order (big endian). */
  for (unsigned i = 0; i < CONFIG_ROM_SIZE; i++)
    crom->field[i] = ntohl(crom->field[i]);

  /* XXX The TI chips I have (104c:8023) seem to reload ConfigROMhdr
     and BusOptions from memory (on bus reset?) and get the byte
     swapping wrong. The result is that block reads on the ConfigROM,
     which are served completely from memory have the correct values
     and quadlet reads return these two words in the wrong byte
     order. As the spec mandates that the ConfigROM must be read using
     quadlet reads, other nodes fail to parse our ConfigROM.

     By byte swapping these two fields in memory, they are fetched in
     the correct order and quadlet reads return the right value. If
     someone reads our ConfigROM using block reads, he now gets these
     two fields byte swapped, but he violates the spec anyway.
  */
  crom->field[0] = ntohl(crom->field[0]);
  crom->field[2] = ntohl(crom->field[2]);

  /* Reload the ConfigROM */
  OHCI_REG(ohci, ConfigROMmap) = (uint32_t)(crom->field);
  OHCI_REG(ohci, HCControlSet) = HCControl_BIBimageValid;

}



static void
wait_loop(struct ohci_controller *ohci, uint32_t reg, uint32_t mask, uint32_t value, uint32_t max_ticks)
{
  unsigned i = 0;

  while ((OHCI_REG(ohci, reg) & mask) != value) {
    wait(1);
    if (i++ > max_ticks) {
      printf("waiting for reg %x mask %x value %x\n", reg, mask, value);
      __exit(0xdeeed);
    }
  }
}

static uint8_t
phy_read(struct ohci_controller *ohci, uint8_t addr)
{
  OHCI_REG(ohci, PhyControl) = PhyControl_Read(addr);
  wait_loop(ohci, PhyControl, PhyControl_ReadDone, PhyControl_ReadDone, PHY_TIMEOUT);

  uint8_t result = PhyControl_ReadData(OHCI_REG(ohci, PhyControl));
  return result;
}

/** Write a OHCI PHY register.
 * \param ohci the host controller.
 * \param addr the address to write to.
 * \param data the data to write.
 */
static void
phy_write(struct ohci_controller *ohci, uint8_t addr, uint8_t data)
{
  OHCI_REG(ohci, PhyControl) = PhyControl_Write(addr, data);
  wait_loop(ohci, PhyControl, PhyControl_WriteDone, 0, PHY_TIMEOUT);
}

static void
phy_page_select(struct ohci_controller *ohci, enum phy_page page, uint8_t port)
{
  assert(ohci->enhanced_phy_map, "no page select possible");
  assert(port < 16, "bad port");
  assert(page <  7, "bad page");

  phy_write(ohci, 7, (page << 5) | port);
}

/** Force a bus reset.
 * \param ohci the host controller.
 */
void
ohci_force_bus_reset(struct ohci_controller *ohci)
{
  uint8_t phy1 = phy_read(ohci, 1);

  /* Enable IBR in Phy 1 */
  /* XXX Set RHB to 0? */
  phy1 |= 1<<6;
  phy_write(ohci, 1, phy1);
}

/** Perform a soft-reset of the Open Host Controller and wait until it
 *  has reinitialized itself.
 */
static void
ohci_softreset(struct ohci_controller *ohci)
{
  OHCI_INFO("Soft-resetting controller...\n");
  OHCI_REG(ohci, HCControlSet) = HCControl_softReset;
  wait_loop(ohci, HCControlSet, HCControl_softReset, 0, RESET_TIMEOUT);
}

/* Check version of controller. Returns true, if it is supported. */
static bool
ohci_check_version(struct ohci_controller *ohci)
{
  /* Check version of OHCI controller. */
  uint32_t ohci_version_reg = OHCI_REG(ohci, Version);

  assert(ohci_version_reg != 0xFFFFFFFF, "Invalid PCI data?");

  uint8_t  ohci_version  = (ohci_version_reg >> 16) & 0xFF;
  uint8_t  ohci_revision = ohci_version_reg & 0xFF;

  if ((ohci_version <= 1) && (ohci_revision < 10)) {
    printf("Controller implements OHCI %d.%d. But we require at least 1.10.\n",
	   ohci_version, ohci_revision);
    return false;
  }

  return true;
}

bool
ohci_initialize(const struct pci_device *pci_dev,
		struct ohci_controller *ohci,
		bool posted_writes,
		enum link_speed speed)
{
  ohci->pci = pci_dev;
  ohci->ohci_regs = (volatile uint32_t *) pci_cfg_read_uint32(ohci->pci, PCI_CFG_BAR0);
  ohci->posted_writes = posted_writes;

  assert((uint32_t)ohci->ohci_regs != 0xFFFFFFFF, "Invalid PCI read?");

  uint32_t vendor = pci_read_uint32(pci_dev->cfg_address + PCI_CFG_VENDOR_ID);
  OHCI_INFO("Controller (%x:%x) = %s.\n",
	    vendor & 0xffff,
	    (vendor >> 16) & 0xffff,
	    pci_dev->db->device_name);

  if (ohci->ohci_regs == NULL) {
    printf("Uh? OHCI register pointer is NULL.\n");
    return false;
  }

  if (!ohci_check_version(ohci)) {
    return false;
  }

  /* Do a softreset. */
  ohci_softreset(ohci);

  /* Disable linkEnable to be able to configure the low level stuff. */
  OHCI_REG(ohci, HCControlClear) = HCControl_linkEnable;
  wait_loop(ohci, HCControlSet, HCControl_linkEnable, 0, MISC_TIMEOUT);

  /* Disable stuff we don't want/need, including byte swapping. */
  OHCI_REG(ohci, HCControlClear) = HCControl_noByteSwapData | HCControl_ackTardyEnable;

  /* Enable (or disable) posted writes. With posted writes enabled, the controller
     may return ack_complete for physical write requests, even if the
     data has not been written yet. For coherency considerations,
     refer to Chapter 3.3.3 in the OHCI spec. */
  OHCI_REG(ohci, posted_writes ? HCControlSet : HCControlClear) = HCControl_postedWriteEnable;

  /* XXX BEGIN CRAP CODE Enable LPS. This is more complicated than it
     should be, but hardware sucks... */
  unsigned lps_retries;
  for (lps_retries = 10; lps_retries > 0; lps_retries--) {
    unsigned wait_more_cnt = 10;
    uint32_t phycontrol, intevent;

    OHCI_REG(ohci, HCControlSet) = HCControl_LPS;
    wait_loop(ohci, HCControlSet, HCControl_LPS, HCControl_LPS, MISC_TIMEOUT);

    wait(50);			/* SCLK should be up by now */

    OHCI_REG(ohci, IntEventClear) = ~0U;
    OHCI_REG(ohci, PhyControl) = PhyControl_Read(1);

  wait_some_more:
    wait(50);

    phycontrol = OHCI_REG(ohci, PhyControl);
    intevent   = OHCI_REG(ohci, IntEventSet);
    if (((phycontrol & PhyControl_ReadDone) == 0) &&
	((intevent   & regAccessFail) == 0)) {
      /* Nothing happened yet. This can mean two things: a) the read
	 is not completed, which is unlikely given that we waited 10ms
	 for it. b) SCLK did not start and the PHY is not
	 available. In the latter case regAccessFail should be set,
	 but this does not seem to work. */
      OHCI_INFO("SCLK seems not to be running.\n");
      /* Start over. */
      goto next;
    }

    if ((intevent & regAccessFail) != 0) {
      /* SCLK has not started yet. Wait some more. */
      OHCI_INFO("regAccessFail while waiting for SCLK to start.\n");
      if (wait_more_cnt-- > 0)
	goto wait_some_more;
      else {
	OHCI_INFO("LPS did not come up.\n");
	return false;
      }
    }

    /* Read done. */
    break;

  next:
    OHCI_INFO("%d retries left.\n", lps_retries);

    /* Disable LPS */
    OHCI_REG(ohci, HCControlClear) = HCControl_LPS;
    OHCI_INFO("Waiting for LPS clear.\n");
    wait_loop(ohci, HCControlSet, HCControl_LPS, 0, MISC_TIMEOUT);
    OHCI_INFO("Done.\n");
  }

  if (lps_retries == 0) {
    OHCI_INFO("LPS did not come up.\n");
    return false;
  }

  /* XXX END CRAP CODE */

  /* Disable contender bit */
  uint8_t phy4 = phy_read(ohci, 4);
  phy_write(ohci, 4, phy4 & ~0x40);

  /* LPS is up. We can now communicate with the PHY. Discover how many
     ports we have and whether this PHY supports the enhanced register
     map. */
  uint8_t phy_2 = phy_read(ohci, 2);
  ohci->total_ports = phy_2 & ((1<<5) - 1);
  ohci->enhanced_phy_map = (phy_2 >> 5) == 7;

  OHCI_INFO("Controller has %d ports and %s enhanced PHY map.\n",
	    ohci->total_ports,
	    ohci->enhanced_phy_map ? "an" : "no");

  if (ohci->enhanced_phy_map) {

    /* Enable all ports. */
    for (unsigned port = 0; port < ohci->total_ports; port++) {

      phy_page_select(ohci, PORT_STATUS, port);
      uint8_t reg0 = phy_read(ohci, 8);

      if ((reg0 & PHY_PORT_DISABLED) != 0) {
	OHCI_INFO("Enabling port %d.\n", port);
	phy_write(ohci, 8, reg0 & ~PHY_PORT_DISABLED);
      }
    }
  }

  /* Check if we are responsible for configuring IEEE1394a
     enhancements. */
  if (OHCI_REG(ohci, HCControlSet) & HCControl_programPhyEnable) {
    OHCI_INFO("Enabling IEEE1394a enhancements.\n");
    OHCI_REG(ohci, HCControlSet) = HCControl_aPhyEnhanceEnable;
    /* XXX We should probably do more here. Check:
       http://git.kernel.org/?p=linux/kernel/git/ieee1394/linux1394-2.6.git;a=commit;h=925e7a6504966b838c519f009086982c68e0666f
    */
  } else {
    OHCI_INFO("IEEE1394a enhancements are already configured.\n");
  }

  // reset Link Control register
  OHCI_REG(ohci, LinkControlClear) = 0xFFFFFFFFU;

  // accept requests from all nodes
  OHCI_REG(ohci, AsReqFilterHiSet) = ~0U;
  OHCI_REG(ohci, AsReqFilterLoSet) = ~0U;

  // accept physical requests from all nodes in our local bus
  OHCI_REG(ohci, PhyReqFilterHiSet) = ~0U;
  OHCI_REG(ohci, PhyReqFilterLoSet) = ~0U;

  // allow access up to 0xffff00000000
  OHCI_REG(ohci, PhyUpperBound) = 0xFFFF0000;
  /* if (OHCI_REG(ohci, PhyUpperBound) == 0) { */
  /*   OHCI_INFO("PhyUpperBound doesn't seem to be implemented. (No cause for alarm.)\n"); */
  /* } */

  /* Set SelfID buffer */
  ohci->selfid_buf = mbi_alloc_protected_memory(multiboot_info, sizeof(uint32_t[504]), 11);
  OHCI_INFO("Allocated SelfID buffer at %p.\n", ohci->selfid_buf);

  ohci->selfid_buf[0] = 0xDEADBEEF; /* error checking */
  OHCI_REG(ohci, SelfIDBuffer) = (uint32_t)ohci->selfid_buf;
  OHCI_REG(ohci, LinkControlSet) = LinkControl_rcvSelfID;

  // we retry because of a busy partner
  OHCI_REG(ohci, ATRetries) = 0xFFF;

  if (OHCI_REG(ohci, HCControlSet) & HCControl_linkEnable) {
    OHCI_INFO("Link is already enabled. Why?!\n");
  }

  /* Set Config ROM */
  ohci->crom = mbi_alloc_protected_memory(multiboot_info, sizeof(ohci_config_rom_t), 10);
  OHCI_INFO("ConfigROM allocated at %p.\n", ohci->crom);

  ohci_generate_crom(ohci, speed);
  ohci_load_crom(ohci);

  /* enable link */
  OHCI_REG(ohci, HCControlSet) = HCControl_linkEnable;

  /* Wait for link to come up. */
  wait_loop(ohci, HCControlSet, HCControl_linkEnable, HCControl_linkEnable, MISC_TIMEOUT);
  OHCI_INFO("Link is up. Force bus reset.\n");

  /* Force bus reset and wait for it to complete and then some more
     for the link to calm down. */
  uint8_t generation = (OHCI_REG(ohci, SelfIDCount) >> 16) & 0xFF;
  unsigned poll_count = 0;
  ohci_force_bus_reset(ohci);

  while (poll_count++ < 1000) {
    ohci_poll_events(ohci);
    wait(1);
  }

  if (generation == ((OHCI_REG(ohci, SelfIDCount) >> 16) & 0xFF))
    OHCI_INFO("No bus reset (or a lot of them)? Things may be b0rken.\n");

  /* Print GUID for easy reference. */
  OHCI_INFO("GUID: 0x%llx\n", (uint64_t)(OHCI_REG(ohci, GUIDHi)) << 32 | OHCI_REG(ohci, GUIDLo));

  return true;
}

/** Handle a bus reset condition. Does not return until the reset is
    handled. */
void
ohci_handle_bus_reset(struct ohci_controller *ohci)
{
  /* We have to clear ContextControl.run */
  OHCI_REG(ohci, AsReqTrContextControlClear) = 1 << 15;
  OHCI_REG(ohci, AsRspTrContextControlClear) = 1 << 15;

  /* Wait for active DMA to finish. (We don't do DMA... ) */
  wait_loop(ohci, AsReqTrContextControlSet, ATactive, 0, 10);
  wait_loop(ohci, AsRspTrContextControlSet, ATactive, 0, 10);

  /* Wait for completion of SelfID phase. */
  assert(OHCI_REG(ohci, LinkControlSet) & LinkControl_rcvSelfID,
	 "selfID receive borken");
  wait_loop(ohci, IntEventSet, selfIDComplete2, selfIDComplete2, 1000);

  /* We are done. Clear bus reset indication bits. */
  OHCI_REG(ohci, IntEventClear) = busReset | selfIDComplete2;

  /* Reset request filters. They are cleared on bus reset. */
  OHCI_REG(ohci, AsReqFilterHiSet) = ~0U;
  OHCI_REG(ohci, AsReqFilterLoSet) = ~0U;
  OHCI_REG(ohci, PhyReqFilterHiSet) = ~0U;
  OHCI_REG(ohci, PhyReqFilterLoSet) = ~0U;

  if (~0U != (OHCI_REG(ohci, PhyReqFilterLoSet) & OHCI_REG(ohci, PhyReqFilterHiSet) &
	      OHCI_REG(ohci, AsReqFilterLoSet)  & OHCI_REG(ohci, AsReqFilterHiSet))) {
    printf("Warning: Your controller seems confused. ReqFilters: 0x%llx 0x%llx\n", 
	   (unsigned long long) OHCI_REG(ohci, PhyReqFilterHiSet) << 32 | OHCI_REG(ohci, PhyReqFilterLoSet),
	   (unsigned long long) OHCI_REG(ohci,  AsReqFilterHiSet) << 32 | OHCI_REG(ohci, AsReqFilterLoSet));
  }

  uint32_t selfid_count = OHCI_REG(ohci, SelfIDCount);
  uint8_t  selfid_words = (selfid_count >> 2) & 0xFF;

  for (unsigned i = 1; i < selfid_words; i += 2) {
    assert(i < sizeof(uint32_t[504])/sizeof(uint32_t), "buffer overflow");
    uint32_t cur = ohci->selfid_buf[i];
    uint32_t next = ohci->selfid_buf[i+1];
    OHCI_INFO("SelfID#%x buf[0x%x] = 0x%x (%s)\n",
              (selfid_count >> 16) & 0xFF, /* Generation */
              i, cur, (cur == ~next) ? "OK" : "CORRUPT");
  }

}

void
ohci_poll_events(struct ohci_controller *ohci)
{
  uint32_t intevent = OHCI_REG(ohci, IntEventSet); /* Unmasked event bitfield */

  if ((intevent & busReset) != 0) {
    OHCI_INFO("Bus reset!\n");
    ohci_handle_bus_reset(ohci);
  } else if ((intevent & postedWriteErr) != 0) {
    OHCI_INFO("Posted Write Error\n");
    OHCI_REG(ohci, IntEventClear) = postedWriteErr;
  } else if ((intevent & unrecoverableError) != 0) {
    OHCI_INFO("Unrecoverable Error\n");
    OHCI_REG(ohci, IntEventClear) = unrecoverableError;
  }
}

/** Wait until we get a valid bus number. */
uint8_t
ohci_wait_nodeid(struct ohci_controller *ohci)
{
  wait_loop(ohci, NodeID, NodeID_idValid, NodeID_idValid, MISC_TIMEOUT);

  uint32_t nodeid_reg = OHCI_REG(ohci, NodeID);
  uint8_t node_number = nodeid_reg & NodeID_nodeNumber;

  return node_number;
}


/* EOF */
