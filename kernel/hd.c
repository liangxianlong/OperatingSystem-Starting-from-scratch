/**
 *	Hard disk driver.
 *
 * 	The `device nr` in this file means minor device nr.
 *
 */
#include "type.h"
#include "stdio.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "console.h"
#include "global.h"
#include "proto.h"
#include "hd.h"

PRIVATE void init_hd	();
PRIVATE void hd_open	(int device);
PRIVATE void hd_close	(int device);
PRIVATE void hd_rdwt	(MESSAGE * p);
PRIVATE void hd_ioctl	(MESSAGE * p);
PRIVATE void hd_cmd_out	(struct hd_cmd* cmd);
PRIVATE void get_part_table		(int drive, int sect_nr, struct part_ent * entry);
PRIVATE void partition	(int device, int style);
PRIVATE int waitfor		(int mask, int val, int timeout);
PRIVATE void interrupt_wait	();
PRIVATE void hd_identify	(int drive);
PRIVATE void print_identify_info	(u16* hdinfo);

PRIVATE u8	hd_status;
PRIVATE u8	hdbuf[SECTOR_SIZE * 2];
PRIVATE struct hd_info	hd_info[1];

#define	DRV_OF_DEV(dev)	(dev <= MAX_PRIM ? dev/NR_PRIM_PER_DRIVE : (dev - MINOR_hd1a) / NR_SUB_PER_DRIVE)

/**
 * task_hd
 *
 * Main loop of HD driver.
 */
PUBLIC void task_hd()
{
	MESSAGE msg;
	
	init_hd();

	while(1) {
		send_recv(RECEIVE, ANY, &msg);

		int src = msg.source;

		switch(msg.type) {
			case DEV_OPEN:
				hd_open(msg.DEVICE);
				break;
			case DEV_CLOSE:
				hd_close(msg.DEVICE);
				break;
			case DEV_READ:
			case DEV_WRITE:
				hd_rdwt(&msg);
				break;
			case DEV_IOCTL:
				hd_ioctl(&msg);
				break;
			default:
				dump_msg("HD driver::unknown msg", &msg);
				spin("FS::main_loop (invalid msg type)");
				break;			
		}

		send_rect(SEND, src, &msg);
	}
}

/**
 * init_hd
 *
 * <Ring 1> Check hard drive, set IRQ handler, enable IRQ and initialize data
 *
 */
PRIVATE void init_hd()
{
	int i;

	// get the number of drivers from the BIOS data area
	u8 * pNrDrives = (u8*)(0x475);
	printl("{HD} pNrDrives:%d.\n", *pNrDrives);
	assert(*pNrDrives);

	put_irq_handler(AT_WINI_IRQ, hd_handler);
	enable_irq(CASCADE_IRQ);
	enable_irq(AT_WINI_IRQ);

	for(i = 0; i < (sizeof(hd_info)/sizeof(hd_info[0])); i++)
		memset(&hd_info[i], 0, sizeof(hd_info[0]));
	hd_info[0].open_cnt = 0;
}

/**
 * hd_open
 *
 * <Ring 1> This routine handles DEV_OPEN message. It identify the drive
 * of the given device and read the partition table of the drive 
 * if it has not been read.
 *
 * @param device: The device to be opened.
 *
 */
PRIVATE void hd_open(int device)
{
	int drive = DRV_OF_DEV(device);
	// only one drive
	asset(drive == 0);

	hd_identify(drive);

	if(hd_info[drive].open_cnt++ == 0) {
		partition(drive * (NR_PART_PER_DRIVE + 1), P_PRIMARY);
	}
}

/**
 * hd_close
 *
 * <Ring 1> This routine handles DEV_READ and DEV_WRITE message.
 *
 * @param p: Message ptr.
 */
PRIVATE void hd_rdwt(MESSAGE * p)
{
	int drive = DRV_OF_DEV(p->DEVICE);

	u64 pos = p->POSITION;

	assert((pos >> SECTOR_SIZE_SHIFT) < (1 << 31));

	// we only allow to R/W from a SECTOR boundary.
	assert((pos & 0x1FF) == 0);

	// pos / SECTOR_SIZE
	u32 sect_nr = (u32)(pos >> SECTOR_SIZE_SHIFT);
	int logidx = (p->DEVICE - MINOR_hd1a) % NR_SUB_PER_DRIVE;
	sect_nr += p->DEVICE < MAX_PRIM ? hd_info[drive].primary[p->DEVICE].base : hd_info[drive].logical[logidx].base;

	struct hd_cmd cmd;
	cmd.features = 0;
	cmd.count = (p->CNT + SECTOR_SIZE - 1)/SECTOR_SIZE;
	cmd.lba_low = sect_nr & 0xFF;
	cmd.lba_mid = (sect_nr >> 8) & 0xFF;
	cmd.lba_high = (sect_nr >> 16) & 0xFF;
	cmd.device = MAKE_DEVICE_REG(1, drive, (sect_nr >> 24)& 0xF);
	cmd.command = (p->type == DEV_READ) ? ATA_READ : ATA_WRITE;
	hd_cmd_out(&cmd);

	int bytes_left = p->CNT;
	void * la = (void*)va2la(p->PROC_NR, p->BUF);

	while(bytes_left) {
		int bytes = min(SECTOR_SIZE, bytes_left);
		if(p->type == DEV_READ) {
			interrupt_wait();
			port_read(REG_DATA, hdbuf, SECTOR_SIZE);
			phys_copy(la, (void*)va2la(TASK_HD, hdbuf), bytes);
		} else {
			if(!waitfor(STATUS_DRQ, STATUS_DRQ, HD_TIMEOUT))
				panic("hd writing error");
			port_write(REG_DATA, la, bytes);

			interrupt_wait();
		}
		bytes_left -= SECTOR_SIZE;
		la += SECTOR_SIZE;
	}
}