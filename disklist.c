
/* This is a utility program for listing SCSI devices and hosts (HBAs)
 * in the Linux operating system. It is applicable to kernel versions
 * 2.6.1 and greater.
 *
 *  Copyright (C) 2003-2017 D. Gilbert
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 */

/* tools code based on lsscsi source code
 * standalone tool compile with gcc -DDISK_LIST_STD_APP -DVER_STR='"VERION STRING ID"' -o disklist disklist.c
 * or gcc -DDISK_LIST_STD_APP -DVER_STR=\"$(date +%Y%m%d)\" -o disklist disklist.c
 */

//#define _XOPEN_SOURCE 600

//#ifndef _GNU_SOURCE
#define _GNU_SOURCE
//#endif

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#ifdef DISK_LIST_STD_APP
#ifndef VER_STR
#define VER_STR "STDAPP"
#endif
#else
#include "common.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/sysmacros.h>
#ifndef major
#include <sys/types.h>
#endif
#include <linux/major.h>
#include <linux/limits.h>
#include <time.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

#include <time.h>


#define LOCAL_PORT 100
#define REMOTE_SRV "10.22.47.69"
#define REMOTE_PORT 8601

static const char * version_str = "0.30  2017/10/24 [svn: r141] Based On Edition";

#define FT_OTHER 0
#define FT_BLOCK 1
#define FT_CHAR 2

#define TRANSPORT_UNKNOWN 0
#define TRANSPORT_SPI 1
#define TRANSPORT_FC 2
#define TRANSPORT_SAS 3
#define TRANSPORT_SAS_CLASS 4
#define TRANSPORT_ISCSI 5
#define TRANSPORT_SBP 6
#define TRANSPORT_USB 7
#define TRANSPORT_ATA 8         /* probably PATA, could be SATA */
#define TRANSPORT_SATA 9        /* most likely SATA */
#define TRANSPORT_FCOE 10
#define TRANSPORT_SRP 11

#ifdef PATH_MAX
#define LMAX_PATH PATH_MAX
#else
#define LMAX_PATH 2048
#endif

#ifdef NAME_MAX
#define LMAX_NAME (NAME_MAX + 1)
#else
#define LMAX_NAME 256
#endif

#define LMAX_DEVPATH (LMAX_NAME + 128)

static int transport_id = TRANSPORT_UNKNOWN;


static const char * sysfsroot = "/sys";
static const char * bus_scsi_devs = "/bus/scsi/devices";
static const char * class_scsi_dev = "/class/scsi_device/";
static const char * scsi_host = "/class/scsi_host/";

static const char * sas_host = "/class/sas_host/";
static const char * sas_phy = "/class/sas_phy/";

static const char * sas_device = "/class/sas_device/";
static const char * sas_end_device = "/class/sas_end_device/";


static const char * dev_dir = "/dev";
static const char * dev_disk_byid_dir = "/dev/disk/by-id";


int trim_string(const char *buf, int len) {
	int leading = 0;
	int tail = 0;
	int ret = 0;
	int i = 0;

	if(len < 1) {
		return 0;
	}

	char *p = (char *)buf;
	while(*p == ' ' && i < len) {
		p++;
		i++;
	}
	leading = i;

	i = 0;
	p = (char *)buf + len - 1;
	while(*p == ' ' && i < len) {
		p--;
		i++;
	}
	tail = i;

	ret = ((leading << 16) & 0xFFFF0000) | ((len - tail - leading) & 0xFFFF);
	return ret;
}


#define MAX_EC 8
char ecdepot[MAX_EC][64];

int get_ec_id(char *ec) {
	int i;
	for(i = 0; i < MAX_EC; i++) {
		if(strcmp(ecdepot[i], ec) == 0) {
			return i;
		}
	}

	// not found
	for(i = 0; i < MAX_EC; i++) {
		if( ecdepot[i][0] == '\0') {
			strcpy(ecdepot[i], ec);
			return i;
		}
	}

	// exhausted
	return -1;
}

struct disk_entry {
	short host;
	short bus; /* channel*/
	short target;
	short lun;

	short vd_idx;
	short pd_idx;
	short ec;
	short slot;

	// some megaraid sas, 2108, slot not equal to did
	short did;

	char SN[32];
	char VER[64];
	char MODEL[64];
	float sizeGB;

	char pcipath[16];
	char pd_type[32]; // disk or enclosure
	char wwn[64]; // non VD
	char peer_addr[64];

	char ec_addr[32];

	char path[16];
	char sgpath[16];
	char disk_type[16];
	char raid_inq[96];

	char connect_type[16];
	short type_usb;
};


void init_disk_entry(struct disk_entry *e) {
	e->host = -1;
	e->bus = -1; /* channel*/
	e->target = -1;
	e->lun = -1;

	e->vd_idx = -1;
	e->pd_idx = -1;
	e->ec = -1;
	e->slot = -1;

	e->did = -1;

	strcpy(e->VER, "NA");
	strcpy(e->SN, "NA");
	strcpy(e->MODEL, "NA");
	e->sizeGB = -1.0f;

	strcpy(e->pcipath, "NA");
	strcpy(e->pd_type, "NA");
	strcpy(e->wwn, "NA");
	strcpy(e->peer_addr, "NA");
	strcpy(e->ec_addr, "NA");

	strcpy(e->path, "NA");
	strcpy(e->sgpath, "NA");

	strcpy(e->disk_type, "NA");
	strcpy(e->raid_inq, "NA");

	strcpy(e->connect_type, "NA");

	e->type_usb = 0;
}

void dump_disk_entry(struct disk_entry *e) {

	printf("%s | %s | ", e->sgpath, e->path);
	printf("%d | %d | %d | ", e->ec, e->slot, e->did);
	printf("%d | %d | ", e->vd_idx, e->pd_idx);
	printf("%s | %s | %s | ", e->wwn, e->peer_addr, e->ec_addr);
	printf("%s | %s | %s | %s | ", e->pcipath, e->pd_type, e->disk_type, e->connect_type);
	printf("%d | %d | %d | %d | ", e->host, e->bus, e->target,  e->lun);
	printf("%.0f | %s | ", e->sizeGB, e->raid_inq);
	printf("%s | %s | %s \n", e->MODEL, e->VER, e->SN);

}

void print_host_pci_path(char *buff, struct disk_entry *e) {
	//
	char s[LMAX_PATH], *p = NULL;
	readlink(buff, s, LMAX_PATH);
	p = s;
	int n = strlen(s);

	//printf("*** %s ***\n", buff);
	do {
		if ( ((p[0] == 'a') && (p[1] == 't') && (p[2] == 'a'))
				|| ((p[0] == 'h') && (p[1] == 'o') && (p[2] == 's'))
				|| ((p[0] == 'n') && (p[1] == 'v') && (p[2] == 'm'))
				|| ((p[0] == 'u') && (p[1] == 's') && (p[2] == 'b')) ) {
			break;
		}
		p++;
	} while(p < (s + n));

	if((p[0] == 'u') && (p[1] == 's') && (p[2] == 'b')) {
		e->type_usb = 1;
	}

	p-= 13;
	p[12] = '\0';
	sprintf(e->pcipath, "%s", p);
}


#define MAX_DISK 128
struct disk_entry sys_disk_depot[MAX_DISK];
struct disk_entry raid_pd_depot[MAX_DISK];
struct disk_entry final_depot[MAX_DISK];
int sys_disk_idx = 0;
int raid_ld_pd_idx = 0;
int final_disk_idx = 0;

char cur_ec[32] = {'\0', };
char last_ec[32] = {'\0', };
int ec_idx = -1;


char smartcmd[MAX_DISK][256];

#include <linux/nvme.h>
int nvme_identify(int fd, int namespace, void *ptr, int cns)
{
	struct nvme_admin_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = nvme_admin_identify;
	cmd.nsid = namespace;
	cmd.addr = (unsigned long)ptr;
	cmd.data_len = 4096;
	cmd.cdw10 = cns;

	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}


#define MAX_NVME 32
int nr_nvme = 0;

#include <dirent.h>

void fix_nvme_ns_smartcmd(char *cmd) {
	int len = strlen(cmd);
	int idx = len;

	while(idx >= 0 && cmd[idx] != 'n') {
		idx--;
	}
	cmd[idx] = '\0';
}


// kernel 3.16 does not have directory /sys/class/nvme
// such old kernel: ls -l /sys/block/
// lrwxrwxrwx 1 root root 0 Aug 14  2018 nvme0n1 -> ../devices/pci0000:00/0000:00:02.0/0000:04:00.0/block/nvme0n1
// lrwxrwxrwx 1 root root 0 Aug 14  2018 nvme1n1 -> ../devices/pci0000:80/0000:80:03.0/0000:84:00.0/block/nvme1n1
// kernel 4.x (4.4 and above) /sys/block/nvme* has different name convention
// # ls -l /sys/block/
// lrwxrwxrwx 1 root root 0 Aug 13  2018 nvme0n1 -> ../devices/pci0000:00/0000:00:02.0/0000:04:00.0/nvme/nvme0/nvme0n1
// # ls -l /sys/class/nvme
// lrwxrwxrwx 1 root root 0 Jul 12  2018 nvme0 -> ../../devices/pci0000:00/0000:00:02.0/0000:04:00.0/nvme/nvme0

int scan_nvme_ns(){
	DIR* dir;
	//char pci_path[16];
	dir = opendir("/sys/class/nvme/");
	if(dir == NULL) {
		// no nvme device
		return 0;
	}

	struct dirent * ent;
	while((ent = readdir(dir)) != NULL) {
		char nvme_id[64];
		if(ent->d_name[0] != 'n') {
			continue;
		}
		sprintf(nvme_id, "/sys/class/nvme/%s", ent->d_name);
		struct dirent * nvme_ns;
		DIR* subdir = opendir(nvme_id);
		if(subdir == NULL) {
			continue;
		}
		while((nvme_ns = readdir(subdir)) != NULL) {
			char nvme_ns_path[64];
			char nvme_dev_name[64];
			if(nvme_ns->d_name[0] == 'n' && nvme_ns->d_name[1] == 'v') {
				sprintf(nvme_ns_path, "/sys/class/nvme/%s/%s", ent->d_name, nvme_ns->d_name);
				sprintf(nvme_dev_name, "/dev/%s", nvme_ns->d_name);

				// do the work
				struct nvme_id_ctrl ctrl;
				struct nvme_id_ns ns;
				int ns_id;
				int fd = open(nvme_dev_name, O_RDONLY);
				if(fd < 0 ) {
					continue;
				}
				if(nvme_identify(fd, 0, &ctrl, 1) < 0) {
					continue;
				}

				ns_id = ioctl(fd, NVME_IOCTL_ID);
				if(ns_id < 0) {
					continue;
				}

				if(nvme_identify(fd, ns_id, &ns, 0) < 0) {
					continue;
				}

				struct disk_entry *e = sys_disk_depot + sys_disk_idx;
				init_disk_entry(e);
				strncpy(e->path, nvme_dev_name, strlen(nvme_dev_name));

				int len = trim_string(ctrl.mn, sizeof(ctrl.mn)) & 0xFFFF;
				snprintf(e->MODEL, len + 1, "%s", ctrl.mn);

				len = trim_string(ctrl.fr, sizeof(ctrl.fr)) & 0xFFFF;
				snprintf(e->VER, len + 1, "%s", ctrl.fr);

				len = trim_string(ctrl.sn, sizeof(ctrl.sn)) & 0xFFFF;
				snprintf(e->SN, len + 1, "%s", ctrl.sn);

				snprintf(e->wwn, 17, "5%02x%02x%02x000000000", ctrl.ieee[2], ctrl.ieee[1], ctrl.ieee[0]);

				strcpy(e->disk_type, "NVME SSD");
				strcpy(e->connect_type, "PCIE");

				// # ls -l /sys/class/nvme/
				// lrwxrwxrwx 1 root root 0 Jul 12  2018 nvme0 -> ../../devices/pci0000:00/0000:00:02.0/0000:04:00.0/nvme/nvme0
				print_host_pci_path(nvme_id, e);
				e->sizeGB = (float) (ns.nsze * 512 / 1000 / 1000 / 1000);
				//printf("*** %f %ld %ld %ld***\n", e->sizeGB, ns.nsze, ns.ncap, ns.nuse);

				sys_disk_idx++;
				nr_nvme++;

				close(fd);
			}
		}
		closedir(subdir);
	}
	closedir(dir);

	return nr_nvme;

}

struct addr_hctl {
        int h;
        int c;
        int t;
        uint64_t l;                     /* Linux word flipped */
        unsigned char lun_arr[8];       /* T10, SAM-5 order */
};

struct addr_hctl filter;
static bool filter_active = false;

struct lsscsi_opts {
        bool classic;
        bool dev_maj_min;        /* --device */
        bool generic;
        bool kname;
        bool protection;        /* data integrity */
        bool protmode;          /* data integrity */
        bool scsi_id;           /* udev derived from /dev/disk/by-id/scsi* */
        bool transport_info;
        bool wwn;
        bool all_info;
        int long_opt;           /* --long */
        int lunhex;
        int ssize;              /* show storage size, once->base 10 (e.g. 3 GB
                                 * twice (or more)->base 2 (e.g. 3.1 GiB) */
        int unit;               /* logical unit (LU) name: from vpd_pg83 */
        int verbose;
};



static const char * scsi_short_device_types[] =
{
        "disk   ", "tape   ", "printer", "process", "worm   ", "cd/dvd ",
        "scanner", "optical", "mediumx", "comms  ", "(0xa)  ", "(0xb)  ",
        "storage", "enclosu", "sim dsk", "opti rd", "bridge ", "osd    ",
        "adi    ", "sec man", "zbc    ", "(0x15) ", "(0x16) ", "(0x17) ",
        "(0x18) ", "(0x19) ", "(0x1a) ", "(0x1b) ", "(0x1c) ", "(0x1e) ",
        "wlun   ", "no dev ",
};






/* Device node list: contains the information needed to match a node with a
 * sysfs class device. */
#define DEV_NODE_LIST_ENTRIES 16
enum dev_type { BLK_DEV, CHR_DEV};

struct dev_node_entry {
       unsigned int maj, min;
       enum dev_type type;
       time_t mtime;
       char name[LMAX_DEVPATH];
};

struct dev_node_list {
       struct dev_node_list *next;
       unsigned int count;
       struct dev_node_entry nodes[DEV_NODE_LIST_ENTRIES];
};
static struct dev_node_list* dev_node_listhead = NULL;

/* WWN here is extracted from /dev/disk/by-id/wwn-<WWN> which is
 * created by udev 60-persistent-storage.rules using ID_WWN_WITH_EXTENSION.
 * The udev ID_WWN_WITH_EXTENSION is the combination of char wwn[17] and
 * char wwn_vendor_extension[17] from struct scsi_id_device. This macro
 * defines the maximum length of char-array needed to store this wwn including
 * the null-terminator.
 */
#define DISK_WWN_MAX_LEN 35

struct disk_wwn_node_entry {
       char wwn[DISK_WWN_MAX_LEN]; /* '0x' + wwn<128-bit> + */
                                   /* <null-terminator> */
       char disk_bname[12];
};

#define DISK_WWN_NODE_LIST_ENTRIES 16
struct disk_wwn_node_list {
       struct disk_wwn_node_list *next;
       unsigned int count;
       struct disk_wwn_node_entry nodes[DISK_WWN_NODE_LIST_ENTRIES];
};


struct item_t {
        char name[LMAX_NAME];
        int ft;
        int d_type;
};

static struct item_t non_sg;
static struct item_t aa_sg;
static struct item_t aa_first;


static char sas_low_phy[LMAX_NAME];
static char sas_hold_end_device[LMAX_NAME];


static char errpath[LMAX_PATH];



#ifdef __GNUC__
static int pr2serr(const char * fmt, ...)
        __attribute__ ((format (printf, 1, 2)));
#else
static int pr2serr(const char * fmt, ...);
#endif



#define NR_VD_STR "Number of Virtual Disks"
#define VD_TITLE_STR "Virtual Drive"
#define VD_NR_COMPO_STR "Number Of Drives"
#define NR_SPAN "Number of Spans"
#define PD_TITLE_STR "PD: "
#define PD_EC_STR "Enclosure Device ID"
#define PD_SLOT_STR "Slot Number"
//  SMC2108 LSI Logic / Symbios Logic MegaRAID SAS 2108 [Liberator] slot number diff with did
#define PD_DID_STR "Device Id"
#define PD_WWN_STR "WWN"
#define PD_SIZE_STR "Raw Size"
#define PD_STATE_STR "Firmware state"
#define PD_FIRMVER_STR "Device Firmware Level"
#define PD_SASADDR_STR "SAS Address(0)"
#define PD_INQ_STR "Inquiry Data"
#define PD_TYPE_STR "Media Type"


void lsi_inq_decode(char *dst, char *buf) {

/*
|       S264NXAH101761MZ7LM120HCFD00D3                            GA38
|             Z4D4DG1CST6000NM0024-1US17Z                         MA88
| S2YNNYAH402930      SAMSUNG MZ7TY128HDHP-00000              MAT0100Q
|             S4D0QT1SST6000NM0024-1HT17Z                     SN05
|   PHDV703202S8150MGNSSDSC2BB120G7R                          N201DL41
|             ZAD0YBW5ST6000NM0115-1YZ110                         DA25
|         17C7K80AFE1CTOSHIBA MG04ACA600E                         FS6D
|   PHDV70820032150MGNSSDSC2BB120G7R                          N201DL42

10.6.16.13
| SEAGATE ST6000NM0034    MS2AZ4D1W48J
| TOSHIBA AL13SEB300      DE11Y4T0A12AFRD6

10.6.194.13
|  ATA     ST4000NM0033-9ZMGA0A            Z1Z4P7WK
: SEAGATE ST300MM0006     LS0AS0K2P81H
:             Z1Z4P7V5ST4000NM0033-9ZM170                         GA0A

Inquiry Data:             Z1Z71Y4YST4000NM0033-9ZM170                         GA67

*/

	int len = 0;
	int srcoff = 0;
	int dstoff = 0;
	int fuse = 0;

	int inqlen = strlen(buf);
	if(inqlen > 60) {
		// LSI 3xxx mode

		fuse = trim_string(buf + 20, 40);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		strncpy(dst, buf + 20 + srcoff, len);
		dstoff += len;
		dst[dstoff++] = ' ';
		dst[dstoff++] = '*';
		dst[dstoff++] = ' ';

		fuse = trim_string(buf + 60, 8);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		strncpy(dst + dstoff, buf + 60 + srcoff, len);
		dstoff += len;
		dst[dstoff++] = ' ';
		dst[dstoff++] = '*';
		dst[dstoff++] = ' ';

		fuse = trim_string(buf, 20);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		//printf("*** %s  %x ***\n", buf + srcoff, fuse);
		strncpy(dst + dstoff, buf + srcoff, len);
		dstoff += len;
		dst[dstoff++] = '\0';
	} else {
		// LSI 2xxx mode

		fuse = trim_string(buf + 8, 16);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		strncpy(dst, buf + 8 + srcoff, len);
		dstoff += len;
		dst[dstoff++] = ' ';
		dst[dstoff++] = '*';
		dst[dstoff++] = ' ';

		fuse = trim_string(buf + 24, 4);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		strncpy(dst + dstoff, buf + 24 + srcoff, len);
		dstoff += len;
		dst[dstoff++] = ' ';
		dst[dstoff++] = '*';
		dst[dstoff++] = ' ';

		fuse = trim_string(buf + 28, 20);
		len = fuse & 0xFFFF;
		srcoff = (fuse >> 16) & 0xFFFF;
		strncpy(dst + dstoff, buf + 28 + srcoff, len);
		dstoff += len;
		dst[dstoff++] = '\0';
	}

}

void mega_jbod_list() {
	char *megactl = "/dev/megaraid_sas_ioctl_node";
	char *megabin = "/usr/sbin/megacli";
	struct stat st;
	if(stat(megactl, &st) != 0) {
		//fprintf(stderr, "NO FILE megactl\n");
		return;
	}
	if(stat(megabin, &st) != 0) {
		//fprintf(stderr, "NO FILE megabin\n");
		return;
	}

	char buf[128];
	char *p = NULL;
	int len = -1;

	FILE* fp = popen ("megacli -pdlist -aall -nolog", "r");

	if(fp == NULL) {
		return;
	}

	struct disk_entry *e = NULL;
	int ec_id = -1;
	int slot_id = -1;
	int did = -1;
	float raw_size = -1;

	while((p = fgets(buf, 128, fp))) {
		len = strlen(buf);
		buf[len - 1] = '\0';

		if(strncmp(buf, PD_EC_STR, strlen(PD_EC_STR)) == 0) {
			sscanf(buf, "Enclosure Device ID: %d", &ec_id);
			//printf("%d | ", ec_id);

			e = raid_pd_depot + raid_ld_pd_idx;
			//raid_ld_pd_idx++;
			init_disk_entry(e);

			e->ec = ec_id;
		}

		if(ec_id < 0) {
			// skip useless lines
			continue;
		}

		if(strncmp(buf, PD_SLOT_STR, strlen(PD_SLOT_STR)) == 0) {
			sscanf(buf, "Slot Number: %d", &slot_id);
			//printf("%d | ", slot_id);
			e->slot = slot_id;
		}

		if(strncmp(buf, PD_DID_STR, strlen(PD_DID_STR)) == 0) {
			sscanf(buf, "Device Id: %d", &did);
			e->did = did;
		}

		if(strncmp(buf, PD_WWN_STR, strlen(PD_WWN_STR)) == 0) {
			//printf("%s | ", buf + 5);
			strncpy(e->wwn, buf + 5, strlen(buf + 5));
		}

		if(strncmp(buf, PD_SIZE_STR, strlen(PD_SIZE_STR)) == 0) {
			//sscanf(buf, "Raw Size: %.3f ", &raw_size);
			raw_size = atof(buf + 10);
			if(strstr(buf, "TB")) {
				raw_size *= 1024.0f;
			}
			//printf("%.3f | ", raw_size);
			e->sizeGB = raw_size * 1.024f * 1.024f * 1.024f;
		}

		if(strncmp(buf, PD_STATE_STR, strlen(PD_STATE_STR)) == 0) {
			//printf("%s | ", buf + 16);
			if( !( (buf[16] == 'J') && (buf[17] == 'B') &&
					(buf[18] == 'O') && (buf[19] == 'D') ) ) {
				init_disk_entry(e);
				ec_id = -1;
				continue;
			}
			//printf("%s | ", buf + 16);
			// skip
		}

		if(strncmp(buf, PD_FIRMVER_STR, strlen(PD_FIRMVER_STR)) == 0) {
			//printf("%s | ", buf + 23);
			strncpy(e->VER, buf + 23, strlen(buf + 23));
		}

		if(strncmp(buf, PD_SASADDR_STR, strlen(PD_SASADDR_STR)) == 0) {
			//printf("%s | ", buf + 18);
			strncpy(e->peer_addr, buf + 18, strlen(buf + 18));
		}


		if(strncmp(buf, PD_INQ_STR, strlen(PD_INQ_STR)) == 0) {
			lsi_inq_decode(e->raid_inq, buf + 14);
		}

		if(strncmp(buf, PD_TYPE_STR, strlen(PD_TYPE_STR)) == 0) {
			//printf("%s\n", buf + 12);
			strncpy(e->pd_type, buf + 12, strlen(buf + 12));
			// skip
			// end of PD parse
			raid_ld_pd_idx++;
			ec_id = -1;
			continue;
		}
	}

	pclose(fp);
}

void mega_ld_list() {
	char *megactl = "/dev/megaraid_sas_ioctl_node";
	char *megabin = "/usr/sbin/megacli";
	struct stat st;
	if(stat(megactl, &st) != 0) {
		//fprintf(stderr, "NO FILE megactl\n");
		return;
	}
	if(stat(megabin, &st) != 0) {
		//fprintf(stderr, "NO FILE megabin\n");
		return;
	}

	char buf[128];
	char *p = NULL;

	int nr_vd = -1;
	int vd_idx = -1;
	int len = -1;
	int did = -1;


	FILE* fp = popen ("megacli -ldpdinfo -aall -nolog", "r");

	if(fp == NULL) {
		return;
	}

	while((p = fgets(buf, 128, fp))) {
		len = strlen(buf);
		buf[len - 1] = '\0';
		//printf("%s\n", p);

		if(strncmp(buf, NR_VD_STR, strlen(NR_VD_STR)) == 0) {
			sscanf(buf, "Number of Virtual Disks: %d", &nr_vd);
			//printf("nr_vd: %d\n", nr_vd);
			break;
		}
	}


	int nr_pd = -1;
	int nr_span = -1;
	int nr_disk_per_span = -1;

	while((p = fgets(buf, 128, fp))) {
		len = strlen(buf);
		buf[len - 1] = '\0';
		//printf("%s\n", p);

		if(strncmp(buf, VD_TITLE_STR, strlen(VD_TITLE_STR)) == 0) {
			sscanf(buf, "Virtual Drive: %d", &vd_idx);
			//printf("vd_idx %d\n", vd_idx);
			// not a MUST here
			nr_pd = -1;
			nr_span = -1;
			nr_disk_per_span = -1;
		}

		if(strncmp(buf, VD_NR_COMPO_STR, strlen(VD_NR_COMPO_STR)) == 0) {
			sscanf(buf, "Number Of Drives    : %d", &nr_pd);
			sscanf(buf, "Number Of Drives per span:%d", &nr_disk_per_span);
			//printf("%s %d\n", buf, nr_disk_per_span);
			//printf("1 - nr_pd: %d\n", nr_pd);
		}

		if(strncmp(buf, NR_SPAN, strlen(NR_SPAN)) == 0) {
			sscanf(buf, "Number of Spans: %d", &nr_span);
			//printf("nr_span: %d\n", nr_span);
		}

		if(nr_disk_per_span > 1 && nr_span > 0) {
			nr_pd = nr_disk_per_span * nr_span;
			//printf("2 - nr_pd: %d\n", nr_pd);
		}


		if(nr_pd > 0) {
			int j = 0;
			int ec_id = -1;
			int slot_id = -1;
			float raw_size = 0.0f;
			struct disk_entry *e = NULL;

			for(j = 0; j < nr_pd; j++) {

				//printf("VD %d - PD %d\n", vd_idx, j);
				//printf("%d | %d | ", vd_idx, j);

				//raid_ld_pd_idx
				e = raid_pd_depot + raid_ld_pd_idx;
				raid_ld_pd_idx++;
				init_disk_entry(e);
				e->vd_idx = vd_idx;
				e->pd_idx = j;


				while((p = fgets(buf, 128, fp))) {
					len = strlen(buf);
					buf[len - 1] = '\0';

					if(strncmp(buf, PD_EC_STR, strlen(PD_EC_STR)) == 0) {
						sscanf(buf, "Enclosure Device ID: %d", &ec_id);
						//printf("%d | ", ec_id);
						e->ec = ec_id;
					}

					if(strncmp(buf, PD_SLOT_STR, strlen(PD_SLOT_STR)) == 0) {
						sscanf(buf, "Slot Number: %d", &slot_id);
						//printf("%d | ", slot_id);
						e->slot = slot_id;
					}


					if(strncmp(buf, PD_DID_STR, strlen(PD_DID_STR)) == 0) {
						sscanf(buf, "Device Id: %d", &did);
						e->did = did;
					}

					if(strncmp(buf, PD_WWN_STR, strlen(PD_WWN_STR)) == 0) {
						//printf("%s | ", buf + 5);
						strncpy(e->wwn, buf + 5, strlen(buf + 5));
					}

					if(strncmp(buf, PD_SIZE_STR, strlen(PD_SIZE_STR)) == 0) {
						//sscanf(buf, "Raw Size: %.3f ", &raw_size);
						raw_size = atof(buf + 10);
						if(strstr(buf, "TB")) {
							raw_size *= 1024.0f;
						}
						//printf("%.3f | ", raw_size);
						e->sizeGB = raw_size * 1.024f * 1.024f * 1.024f;
					}

					if(strncmp(buf, PD_STATE_STR, strlen(PD_STATE_STR)) == 0) {
						//printf("%s | ", buf + 16);
						// skip
					}

					if(strncmp(buf, PD_FIRMVER_STR, strlen(PD_FIRMVER_STR)) == 0) {
						//printf("%s | ", buf + 23);
						strncpy(e->VER, buf + 23, strlen(buf + 23));
					}

					if(strncmp(buf, PD_SASADDR_STR, strlen(PD_SASADDR_STR)) == 0) {
						//printf("%s | ", buf + 18);
						strncpy(e->peer_addr, buf + 18, strlen(buf + 18));
					}

					if(strncmp(buf, PD_INQ_STR, strlen(PD_INQ_STR)) == 0) {
						lsi_inq_decode(e->raid_inq, buf + 14);
					}

					if(strncmp(buf, PD_TYPE_STR, strlen(PD_TYPE_STR)) == 0) {
						//printf("%s\n", buf + 12);
						strncpy(e->pd_type, buf + 12, strlen(buf + 12));
						// skip
						// end of PD parse
						break;
					}
				} // end per pd parsing

			} // end all pd

			// restore nr_pd and related to -1, a MUST here
			nr_pd = -1;
			nr_span = -1;
			nr_disk_per_span = -1;

		} // end pd list

	} // end all vd

	pclose(fp);
}



void swap(char *p, int len) {
	int i = 0;
	char c;
	for (i = 0; i < len;) {
		c = p[i + 1];
		p[i + 1] = p[i];
		p[i] = c;
		i += 2;
	}
}

int vpd80(const char *devicefile, struct disk_entry *e)
{
   const char*  file_name = NULL, *ptr = NULL;
   int  sg_fd;
   char  reply_buffer[572];
   char  sense_buffer[64];
   char buf[64];
   sg_io_hdr_t  io_hdr;

   unsigned char  inquiry[6]
    = {0x12, 0x01, 0x80, 0x00, 0x3c, 0x00};

   if(e->type_usb) {
	   return 1;
   }

   /* Open device file */
   file_name = devicefile;
   if ((sg_fd = open(file_name, O_RDONLY)) < 0)
   {
      fprintf(stderr, "Cannot open devicefile %s!\n\n", devicefile);
      return 1;
   }

   /* Send INQUIRY command */
   memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
   io_hdr.interface_id = 'S';
   io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
   io_hdr.mx_sb_len = sizeof(sense_buffer);
   io_hdr.sbp = (unsigned char *)sense_buffer;
   io_hdr.dxfer_len = sizeof(reply_buffer);
   io_hdr.dxferp = reply_buffer;
   io_hdr.cmd_len = sizeof(inquiry);
   io_hdr.cmdp = inquiry;
   io_hdr.timeout = 10000;  /* Miliseconds */
   if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
   {
      fprintf(stderr, "Cannot send INQUIRY command!\n\n");
      close(sg_fd);
      return 1;
   }

   if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK)
   {
	  close(sg_fd);
      return 1;
   }

   /* Extract SN */
   if ((reply_buffer[1] & 0xFF) != 0x80)
   {
      fprintf(stderr, "Unit serial number page invalid! %x\n",
    		  reply_buffer[1]);
      close(sg_fd);
      return 1;
   }

   int rawret = 0;
   int srclen = 0;
   int srcoff = 0;

   // 20 bytes SN in standard SPC SAS Inq, snprintf len include the last '\0', plus 1 needed
   snprintf(buf, 20 + 1, "%s", reply_buffer + 4);
   // no need swap as SATA inq
   //swap(buf, 20);
   rawret = trim_string(buf, 20);
   srclen = rawret & 0xFFFF;
   srcoff = (rawret >> 16) & 0xFFFF;
   ptr = buf + srcoff;
   snprintf(e->SN, srclen + 1, "%s", ptr);
    //printf(" *** %s + %s", buf, e->SN);

   close(sg_fd);
   return 0;
}


int vpd89(const char *devicefile, struct disk_entry *e)
{
   const char*  file_name = NULL, *ptr = NULL;
   int  sg_fd;
   char  reply_buffer[572];
   char  sense_buffer[64];
   char buf[64];
   sg_io_hdr_t  io_hdr;

   unsigned char  inquiry[6]
    = {0x12, 0x01, 0x89, 0x02, 0x3c, 0x00};

   if(e->type_usb) {
	   return 1;
   }

   /* Open device file */
   file_name = devicefile;
   if ((sg_fd = open(file_name, O_RDONLY)) < 0)
   {
      fprintf(stderr, "Cannot open devicefile %s!\n\n", devicefile);
      return 1;
   }

   /* Send INQUIRY command */
   memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
   io_hdr.interface_id = 'S';
   io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
   io_hdr.mx_sb_len = sizeof(sense_buffer);
   io_hdr.sbp = (unsigned char *)sense_buffer;
   io_hdr.dxfer_len = sizeof(reply_buffer);
   io_hdr.dxferp = reply_buffer;
   io_hdr.cmd_len = sizeof(inquiry);
   io_hdr.cmdp = inquiry;
   io_hdr.timeout = 10000;  /* Miliseconds */
   if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
   {
      fprintf(stderr, "Cannot send INQUIRY command!\n\n");
      close(sg_fd);
      return 1;
   }

   if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK)
   {
	  /*
      fprintf(stderr, "INQUIRY command failed!\n");
      if (io_hdr.sb_len_wr > 0)
      {
         fprintf(stderr, "Sense data: ");
         for (i = 0; i < io_hdr.sb_len_wr; i++)
         {
        	 fprintf(stderr, "0x%02X ", sense_buffer[i]);
         }
         fprintf(stderr, "\n");
      }
      */
	  close(sg_fd);
      return 1;
   }

   /* Extract SN */
   if ((reply_buffer[1] & 0xFF) != 0x89)
   {
      fprintf(stderr, "Unit serial number page invalid! %x\n",
    		  reply_buffer[1]);
      close(sg_fd);
      return 1;
   }


   /*
   Table 21 — IDENTIFY DEVICE data (part 1 of 10)
   10-19 M B F Serial number (20 ASCII characters)
   20-21 X Retired
   22 X Obsolete
   23-26 M B F Firmware revision (8 ASCII characters)
   27-46 M B F Model number (40 ASCII characters)
   */

   int rawret = 0;
   int srclen = 0;
   int srcoff = 0;

   snprintf(buf, 20 + 1, "%s", reply_buffer + 60 + 10 * 2);
   swap(buf, 20);
   rawret = trim_string(buf, 20);
   srclen = rawret & 0xFFFF;
   srcoff = (rawret >> 16) & 0xFFFF;
   ptr = buf + srcoff;
   snprintf(e->SN, srclen + 1, "%s", ptr);
    //printf(" *** %s + %s", buf, e->SN);


   snprintf(buf, 8 + 1, "%s", reply_buffer + 60 + 23 * 2);
   swap(buf, 8);
   rawret = trim_string(buf, 8);
   //printf(" *** %s + %x", buf, rawret);
   srclen = rawret & 0xFFFF;
   srcoff = (rawret >> 16) & 0xFFFF;
   ptr = buf + srcoff;
   snprintf(e->VER, srclen + 1, "%s", ptr);


   snprintf(buf, 40 + 1, "%s", reply_buffer + 60 + 27 * 2);
   swap(buf, 40);
   rawret = trim_string(buf, 40);
   srclen = rawret & 0xFFFF;
   srcoff = (rawret >> 16) & 0xFFFF;
   ptr = buf + srcoff;
   snprintf(e->MODEL, srclen + 1, "%s", ptr);

   close(sg_fd);
   return 0;
}



int vpdb1(const char *devicefile, struct disk_entry *e)
{
   const char*  file_name = NULL, *ptr = NULL;
   int  sg_fd;
   char  reply_buffer[572];
   char  sense_buffer[64];
   char buf[64];
   sg_io_hdr_t  io_hdr;

   unsigned char  inquiry[6]
    = {0x12, 0x01, 0xb1, 0x00, 0x3c, 0x00};

   if(e->type_usb) {
	   return 1;
   }

   /* Open device file */
   file_name = devicefile;
   if ((sg_fd = open(file_name, O_RDONLY)) < 0)
   {
      fprintf(stderr, "Cannot open devicefile %s!\n\n", devicefile);
      return 1;
   }

   /* Send INQUIRY command */
   memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
   io_hdr.interface_id = 'S';
   io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
   io_hdr.mx_sb_len = sizeof(sense_buffer);
   io_hdr.sbp = (unsigned char *)sense_buffer;
   io_hdr.dxfer_len = sizeof(reply_buffer);
   io_hdr.dxferp = reply_buffer;
   io_hdr.cmd_len = sizeof(inquiry);
   io_hdr.cmdp = inquiry;
   io_hdr.timeout = 10000;  /* Miliseconds */
   if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
   {
      fprintf(stderr, "Cannot send INQUIRY command!\n\n");
      close(sg_fd);
      return 1;
   }

   if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK)
   {
	  close(sg_fd);
      return 1;
   }

   /* Extract Disk Type */
   if ((reply_buffer[1] & 0xFF) != 0xb1)
   {
      fprintf(stderr, "vpd b1 Response code error %x\n",
    		  reply_buffer[1]);
      close(sg_fd);
      return 1;
   }

   unsigned char bt1 = *(unsigned char *)(reply_buffer + 4);
   unsigned char bt2 = *(unsigned char *)(reply_buffer + 5);
   unsigned short rpm = bt1 * 256 + bt2;

   if(rpm == 1) {
	   sprintf(e->disk_type, "SSD");
   } else {
	   sprintf(e->disk_type, "HDD");
   }

   close(sg_fd);
   return 0;
}

int readCap(const char *devicefile, struct disk_entry *e) {
    // 9e 10 00 00 00 00 00 00 00 00 00 00 00 20 00 00

   const char*  file_name = NULL, *ptr = NULL;
   int  sg_fd;
   char  reply_buffer[572];
   char  sense_buffer[64];
   char buf[64];
   sg_io_hdr_t  io_hdr;

   int i = 0;

   unsigned char  inquiry[16]
    = {0x9e, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00
    };

   if(e->type_usb) {
	   return 1;
   }

   /* Open device file */
   file_name = devicefile;
   if ((sg_fd = open(file_name, O_RDONLY)) < 0)
   {
      fprintf(stderr, "Cannot open devicefile %s!\n\n", devicefile);
      return 1;
   }

   /* Send INQUIRY command */
   memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
   io_hdr.interface_id = 'S';
   io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
   io_hdr.mx_sb_len = sizeof(sense_buffer);
   io_hdr.sbp = (unsigned char *)sense_buffer;
   io_hdr.dxfer_len = sizeof(reply_buffer);
   io_hdr.dxferp = reply_buffer;
   io_hdr.cmd_len = sizeof(inquiry);
   io_hdr.cmdp = inquiry;
   io_hdr.timeout = 10000;  /* Miliseconds */
   if (ioctl(sg_fd, SG_IO, &io_hdr) < 0)
   {
      fprintf(stderr, "Cannot send INQUIRY command!\n\n");
      close(sg_fd);
      return 1;
   }

   if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK)
   {
	  close(sg_fd);
      return 1;
   }

   // error code?

   unsigned long long blks = 0;
   unsigned long long bytes = 0;
   unsigned char *p = (unsigned char *) &blks;
   for(i = 0; i < 8; i++) {
	   p[7 - i] = (unsigned char)(reply_buffer[i]);
   }

   p = (unsigned char *) &bytes;
   for(i = 8; i < 12; i++) {
	   p[11 - i] = (unsigned char)(reply_buffer[i]);
   }

   bytes *= blks;
   e->sizeGB = (double)(bytes / 1000 / 1000 / 1000);
   //printf("%lld %f\n", bytes, e->sizeGB);

   close(sg_fd);
   return 0;
}




static int
pr2serr(const char * fmt, ...)
{
        va_list args;
        int n;

        va_start(args, fmt);
        n = vfprintf(stderr, fmt, args);
        va_end(args);
        return n;
}


/* Copies (dest_maxlen - 1) or less chars from src to dest. Less chars are
 * copied if '\0' char found in src. As long as dest_maxlen > 0 then dest
 * will be '\0' terminated on exit. If dest_maxlen < 1 then does nothing. */
static void
my_strcopy(char *dest, const char *src, int dest_maxlen)
{
        const char * lp;

        if (dest_maxlen < 1)
                return;
        lp = (const char *)memchr(src, 0, dest_maxlen);
        if (NULL == lp) {
                memcpy(dest, src, dest_maxlen - 1);
                dest[dest_maxlen - 1] = '\0';
        } else
                memcpy(dest, src, (lp - src) + 1);
}


enum string_size_units {
        STRING_UNITS_10,        /* use powers of 10^3 (standard SI) */
        STRING_UNITS_2,         /* use binary powers of 2^10 */
};


/* Compare <host:controller:target:lun> tuples (aka <h:c:t:l> or hctl) */
static int
cmp_hctl(const struct addr_hctl * le, const struct addr_hctl * ri)
{
        if (le->h == ri->h) {
                if (le->c == ri->c) {
                        if (le->t == ri->t)
                                return ((le->l == ri->l) ? 0 :
                                        ((le->l < ri->l) ? -1 : 1));
                        else
                                return (le->t < ri->t) ? -1 : 1;
                } else
                        return (le->c < ri->c) ? -1 : 1;
        } else
                return (le->h < ri->h) ? -1 : 1;
}


/* Return 1 for directory entry that is link or directory (other than
 * a directory name starting with dot). Else return 0.
 */
static int
first_dir_scan_select(const struct dirent * s)
{
        if (FT_OTHER != aa_first.ft)
                return 0;
        if ((DT_LNK != s->d_type) &&
            ((DT_DIR != s->d_type) || ('.' == s->d_name[0])))
                return 0;
        my_strcopy(aa_first.name, s->d_name, LMAX_NAME);
        aa_first.ft = FT_CHAR;  /* dummy */
        aa_first.d_type =  s->d_type;
        return 1;
}

static int
sub_dir_scan_select(const struct dirent * s)
{
        if (s->d_type == DT_LNK)
                return 1;

        if (s->d_type == DT_DIR && s->d_name[0] != '.')
                return 1;

        return 0;
}


/* Return 1 for directory entry that is link or directory (other than a
 * directory name starting with dot) that contains "block". Else return 0.
 */
static int
block_dir_scan_select(const struct dirent * s)
{
        if (s->d_type != DT_LNK && s->d_type != DT_DIR)
                return 0;

        if (s->d_name[0] == '.')
                return 0;

        if (strstr(s->d_name, "block"))
                return 1;

        return 0;
}

typedef int (* dirent_select_fn) (const struct dirent *);

static int
sub_scan(char * dir_name, const char * sub_str, dirent_select_fn fn)
{
        int num, i, len;
        struct dirent ** namelist;

        num = scandir(dir_name, &namelist, fn, NULL);
        if (num <= 0)
                return 0;
        len = strlen(dir_name);
        if (len >= LMAX_PATH)
                return 0;
        snprintf(dir_name + len, LMAX_PATH - len, "/%s", namelist[0]->d_name);

        for (i = 0; i < num; i++)
                free(namelist[i]);
        free(namelist);

        if (num && strstr(dir_name, sub_str) == 0) {
                num = scandir(dir_name, &namelist, sub_dir_scan_select, NULL);
                if (num <= 0)
                        return 0;
                len = strlen(dir_name);
                if (len >= LMAX_PATH)
                        return 0;
                snprintf(dir_name + len, LMAX_PATH - len, "/%s",
                         namelist[0]->d_name);

                for (i = 0; i < num; i++)
                        free(namelist[i]);
                free(namelist);
        }
        return 1;
}

/* Scan for block:sdN or block/sdN directory in
 * /sys/bus/scsi/devices/h:c:i:l
 */
static int
block_scan(char * dir_name)
{
        return sub_scan(dir_name, "block:", block_dir_scan_select);
}


/* scan for directory entry that is either a symlink or a directory */
static int
scan_for_first(const char * dir_name, const struct lsscsi_opts * op)
{
        int num, k;
        struct dirent ** namelist;

        aa_first.ft = FT_OTHER;
        num = scandir(dir_name, &namelist, first_dir_scan_select, NULL);
        if (num < 0) {
                if (op->verbose > 0) {
                        snprintf(errpath, LMAX_PATH, "scandir: %s", dir_name);
                        perror(errpath);
                }
                return -1;
        }
        for (k = 0; k < num; ++k)
                free(namelist[k]);
        free(namelist);
        return num;
}

static int
non_sg_dir_scan_select(const struct dirent * s)
{
        int len;

        if (FT_OTHER != non_sg.ft)
                return 0;
        if ((DT_LNK != s->d_type) &&
            ((DT_DIR != s->d_type) || ('.' == s->d_name[0])))
                return 0;
        if (0 == strncmp("scsi_changer", s->d_name, 12)) {
                my_strcopy(non_sg.name, s->d_name, LMAX_NAME);
                non_sg.ft = FT_CHAR;
                non_sg.d_type =  s->d_type;
                return 1;
        } else if (0 == strncmp("block", s->d_name, 5)) {
                my_strcopy(non_sg.name, s->d_name, LMAX_NAME);
                non_sg.ft = FT_BLOCK;
                non_sg.d_type =  s->d_type;
                return 1;
        } else if (0 == strcmp("tape", s->d_name)) {
                my_strcopy(non_sg.name, s->d_name, LMAX_NAME);
                non_sg.ft = FT_CHAR;
                non_sg.d_type =  s->d_type;
                return 1;
        } else if (0 == strncmp("scsi_tape:st", s->d_name, 12)) {
                len = strlen(s->d_name);
                if (isdigit(s->d_name[len - 1])) {
                        /* want 'st<num>' symlink only */
                        my_strcopy(non_sg.name, s->d_name, LMAX_NAME);
                        non_sg.ft = FT_CHAR;
                        non_sg.d_type =  s->d_type;
                        return 1;
                } else
                        return 0;
        } else if (0 == strncmp("onstream_tape:os", s->d_name, 16)) {
                my_strcopy(non_sg.name, s->d_name, LMAX_NAME);
                non_sg.ft = FT_CHAR;
                non_sg.d_type =  s->d_type;
                return 1;
        } else
                return 0;
}

static int
non_sg_scan(const char * dir_name, const struct lsscsi_opts * op)
{
        int num, k;
        struct dirent ** namelist;

        non_sg.ft = FT_OTHER;
        num = scandir(dir_name, &namelist, non_sg_dir_scan_select, NULL);
        if (num < 0) {
                if (op->verbose > 0) {
                        snprintf(errpath, LMAX_PATH, "scandir: %s", dir_name);
                        perror(errpath);
                }
                return -1;
        }
        for (k = 0; k < num; ++k)
                free(namelist[k]);
        free(namelist);
        return num;
}


static int
sg_dir_scan_select(const struct dirent * s)
{
        if (FT_OTHER != aa_sg.ft)
                return 0;
        if ((DT_LNK != s->d_type) &&
            ((DT_DIR != s->d_type) || ('.' == s->d_name[0])))
                return 0;
        if (0 == strncmp("scsi_generic", s->d_name, 12)) {
                my_strcopy(aa_sg.name, s->d_name, LMAX_NAME);
                aa_sg.ft = FT_CHAR;
                aa_sg.d_type =  s->d_type;
                return 1;
        } else
                return 0;
}

static int
sg_scan(const char * dir_name)
{
        int num, k;
        struct dirent ** namelist;

        aa_sg.ft = FT_OTHER;
        num = scandir(dir_name, &namelist, sg_dir_scan_select, NULL);
        if (num < 0)
                return -1;
        for (k = 0; k < num; ++k)
                free(namelist[k]);
        free(namelist);
        return num;
}


static int
sas_low_phy_dir_scan_select(const struct dirent * s)
{
        int n, m;
        char * cp;

        if ((DT_LNK != s->d_type) && (DT_DIR != s->d_type))
                return 0;
        if (0 == strncmp("phy", s->d_name, 3)) {
                if (0 == strlen(sas_low_phy))
                        my_strcopy(sas_low_phy, s->d_name, LMAX_NAME);
                else {
                        cp = (char *)strrchr(s->d_name, ':');
                        if (NULL == cp)
                                return 0;
                        n = atoi(cp + 1);
                        cp = strrchr(sas_low_phy, ':');
                        if (NULL == cp)
                                return 0;
                        m = atoi(cp + 1);
                        if (n < m)
                                my_strcopy(sas_low_phy, s->d_name, LMAX_NAME);
                }
                return 1;
        } else
                return 0;
}

static int
sas_low_phy_scan(const char * dir_name, struct dirent ***phy_list)
{
        int num, k;
        struct dirent ** namelist=NULL;

        memset(sas_low_phy, 0, sizeof(sas_low_phy));
        num = scandir(dir_name, &namelist, sas_low_phy_dir_scan_select, NULL);
        if (num < 0)
                return -1;
        if (!phy_list) {
                for (k=0; k<num; ++k)
                        free(namelist[k]);
                free(namelist);
        }
        else
                *phy_list = namelist;
        return num;
}



/* If 'dir_name'/'base_name' is a directory chdir to it. If that is successful
   return true, else false */
static bool
if_directory_chdir(const char * dir_name, const char * base_name)
{
        char b[LMAX_PATH];
        struct stat a_stat;

        snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
        if (stat(b, &a_stat) < 0)
                return false;
        if (S_ISDIR(a_stat.st_mode)) {
                if (chdir(b) < 0)
                        return false;
                return true;
        }
        return false;
}

/* If 'dir_name'/generic is a directory chdir to it. If that is successful
   return true. Otherwise look a directory of the form
   'dir_name'/scsi_generic:sg<n> and if found chdir to it and return true.
   Otherwise return false. */
static bool
if_directory_ch2generic(const char * dir_name)
{
        const char * old_name = "generic";
        char b[LMAX_PATH];
        struct stat a_stat;

        snprintf(b, sizeof(b), "%s/%s", dir_name, old_name);
        if ((stat(b, &a_stat) >= 0) && S_ISDIR(a_stat.st_mode)) {
                if (chdir(b) < 0)
                        return false;
                return true;
        }
        /* No "generic", so now look for "scsi_generic:sg<n>" */
        if (1 != sg_scan(dir_name))
                return false;
        snprintf(b, sizeof(b), "%s/%s", dir_name, aa_sg.name);
        if (stat(b, &a_stat) < 0)
                return false;
        if (S_ISDIR(a_stat.st_mode)) {
                if (chdir(b) < 0)
                        return false;
                return true;
        }
        return false;
}


/* If 'dir_name'/'base_name' is found places corresponding value in 'value'
 * and returns true . Else returns false.
 */
static bool
get_value(const char * dir_name, const char * base_name, char * value,
          int max_value_len)
{
        int len;
        FILE * f;
        char b[LMAX_PATH];

        snprintf(b, sizeof(b), "%s/%s", dir_name, base_name);
        //printf(" **%s %s** ", dir_name, base_name);
        if (NULL == (f = fopen(b, "r"))) {
                return false;
        }
        if (NULL == fgets(value, max_value_len, f)) {
                /* assume empty */
                value[0] = '\0';
                fclose(f);
                return true;
        }
        len = strlen(value);
        if ((len > 0) && (value[len - 1] == '\n'))
                value[len - 1] = '\0';
        fclose(f);
        return true;
}



/* Free dev_node_list. */
static void
free_dev_node_list(void)
{
        if (dev_node_listhead) {
                struct dev_node_list *cur_list, *next_list;

                cur_list = dev_node_listhead;
                while (cur_list) {
                        next_list = cur_list->next;
                        free(cur_list);
                        cur_list = next_list;
                }

                dev_node_listhead = NULL;
        }
}




/*
 * Look up a device node in a directory with symlinks to device nodes.
 * @dir: Directory to examine, e.g. "/dev/disk/by-id".
 * @pfx: Prefix of the symlink, e.g. "scsi-".
 * @dev: Device node to look up, e.g. "/dev/sda".
 * Returns a pointer to the name of the symlink without the prefix if a match
 * has been found. The caller must free the pointer returned by this function.
 * Side effect: changes the working directory to @dir.
 */
static char *
lookup_dev(const char *dir, const char *pfx, const char *dev)
{
        unsigned st_rdev;
        DIR *dirp;
        struct dirent *entry;
        char *result = NULL;
        struct stat stats;

        if (stat(dev, &stats) < 0)
                goto out;
        st_rdev = stats.st_rdev;
        if (chdir(dir) < 0)
                goto out;
        dirp = opendir(dir);
        if (!dirp)
                goto out;
        while ((entry = readdir(dirp)) != NULL) {
                if (stat(entry->d_name, &stats) >= 0 &&
                    stats.st_rdev == st_rdev &&
                    strncmp(entry->d_name, pfx, strlen(pfx)) == 0) {
                        result = strdup(entry->d_name + strlen(pfx));
                        break;
                }
        }
        closedir(dirp);
out:
        return result;
}

/*
 * Obtain the SCSI ID of a disk.
 * @dev_node: Device node of the disk, e.g. "/dev/sda".
 * Return value: pointer to the SCSI ID if lookup succeeded or NULL if lookup
 * failed.
 * The caller must free the returned buffer with free().
 */
char *
get_disk_scsi_id(const char *dev_node)
{
        char *scsi_id = NULL;
        DIR *dir;
        struct dirent *entry;
        char holder[LMAX_PATH + 6];
        char sys_block[LMAX_PATH];

        scsi_id = lookup_dev(dev_disk_byid_dir, "scsi-", dev_node);
        if (scsi_id)
                goto out;
        scsi_id = lookup_dev(dev_disk_byid_dir, "dm-uuid-mpath-", dev_node);
        if (scsi_id)
                goto out;
        scsi_id = lookup_dev(dev_disk_byid_dir, "usb-", dev_node);
        if (scsi_id)
                goto out;
        snprintf(sys_block, sizeof(sys_block), "%s/class/block/%s/holders",
                 sysfsroot, dev_node + 5);
        dir = opendir(sys_block);
        if (!dir)
                goto out;
        while ((entry = readdir(dir)) != NULL) {
                snprintf(holder, sizeof(holder), "/dev/%s", entry->d_name);
                scsi_id = get_disk_scsi_id(holder);
                if (scsi_id)
                        break;
        }
        closedir(dir);
out:
        return scsi_id;
}

/* Fetch USB device name string (form "<b>-<p1>[.<p2>]+:<c>.<i>") given
 * either a SCSI host name or devname (i.e. "h:c:t:l") string. If detected
 * return 'b' (pointer to start of USB device name string which is null
 * terminated), else return NULL.
 */
static char *
get_usb_devname(const char * hname, const char * devname, char * b, int b_len)
{
        int len;
        char * c2p;
        char * cp;
        const char * np;
        char bf2[LMAX_PATH];
        char buff[LMAX_DEVPATH];

        if (hname) {
                snprintf(buff, sizeof(buff), "%s%s", sysfsroot, scsi_host);
                np = hname;
        } else if (devname) {
                snprintf(buff, sizeof(buff), "%s%s", sysfsroot,
                         class_scsi_dev);
                np = devname;
        } else
                return NULL;
        if (if_directory_chdir(buff, np) && getcwd(bf2, sizeof(bf2)) &&
            strstr(bf2, "usb")) {
                if (b_len > 0)
                        b[0] = '\0';
                if ((cp = strstr(bf2, "/host"))) {
                        len = (cp - bf2) - 1;
                        if ((len > 0) &&
                            ((c2p = (char *)memrchr(bf2, '/', len)))) {
                                len = cp - ++c2p;
                                snprintf(b, b_len, "%.*s", len, c2p);
                        }
                }
                return b;
        }
        return NULL;
}

#define VPD_DEVICE_ID 0x83
#define VPD_ASSOC_LU 0
#define VPD_ASSOC_TPORT 1
#define TPROTO_ISCSI 5

/* Iterates to next designation descriptor in the device identification
 * VPD page. The 'initial_desig_desc' should point to start of first
 * descriptor with 'page_len' being the number of valid bytes in that
 * and following descriptors. To start, 'off' should point to a negative
 * value, thereafter it should point to the value yielded by the previous
 * call. If 0 returned then 'initial_desig_desc + *off' should be a valid
 * descriptor; returns -1 if normal end condition and -2 for an abnormal
 * termination. Matches association, designator_type and/or code_set when
 * any of those values are greater than or equal to zero. */
int
sg_vpd_dev_id_iter(const unsigned char * initial_desig_desc, int page_len,
                   int * off, int m_assoc, int m_desig_type, int m_code_set)
{
    const unsigned char * bp;
    int k, c_set, assoc, desig_type;

    for (k = *off, bp = initial_desig_desc ; (k + 3) < page_len; ) {
        k = (k < 0) ? 0 : (k + bp[k + 3] + 4);
        if ((k + 4) > page_len)
            break;
        c_set = (bp[k] & 0xf);
        if ((m_code_set >= 0) && (m_code_set != c_set))
            continue;
        assoc = ((bp[k + 1] >> 4) & 0x3);
        if ((m_assoc >= 0) && (m_assoc != assoc))
            continue;
        desig_type = (bp[k + 1] & 0xf);
        if ((m_desig_type >= 0) && (m_desig_type != desig_type))
            continue;
        *off = k;
        return 0;
    }
    return (k == page_len) ? -1 : -2;
}

/* Fetch logical unit (LU) name given the device name in the
 * form: h:c:t:l tuple string (e.g. "2:0:1:0"). This is fetched via sysfs
 * (lk 3.15 and later) in vpd_pg83. For later ATA and SATA devices this
 * may be its WWN. Normally take the first found in this order:
 * NAA, EUI-64 then SCSI name string. However if a SCSI name string
 * is present and the protocol is iSCSI (target port checked) then
 * the SCSI name string is preferred. If none of the above are present
 * then check for T10 Vendor ID (designator_type=1) and use if available.
 */
static char *
get_lu_name(const char * devname, char * b, int b_len, bool want_prefix)
{
        int fd, res, len, dlen, sns_dlen, off, k, n;
        unsigned char *bp;
        char *cp;
        char buff[LMAX_DEVPATH];
        unsigned char u[512];
        unsigned char u_sns[512];
        struct stat a_stat;

        if ((NULL == b) || (b_len < 1))
                return b;
        b[0] = '\0';
        snprintf(buff, sizeof(buff), "%s%s%s/device/vpd_pg83",
                 sysfsroot, class_scsi_dev, devname);
        if (! ((stat(buff, &a_stat) >= 0) && S_ISREG(a_stat.st_mode)))
                return b;
        if ((fd = open(buff, O_RDONLY)) < 0)
                return b;
        res = read(fd, u, sizeof(u));
        if (res <= 8) {
                close(fd);
                return b;
        }
        close(fd);
        if (VPD_DEVICE_ID != u[1])
                return b;
        len = (u[2] << 8) + u[3];
        if ((len + 4) != res)
                return b;
        bp = u + 4;
        cp = b;
        off = -1;
        if (0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_LU,
                                    8 /* SCSI name string (sns) */,
                                    3 /* UTF-8 */)) {
                sns_dlen = bp[off + 3];
                memcpy(u_sns, bp + off + 4, sns_dlen);
                /* now want to check if this is iSCSI */
                off = -1;
                if (0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_TPORT,
                                            8 /* SCSI name string (sns) */,
                                            3 /* UTF-8 */)) {
                        if ((0x80 & bp[1]) &&
                            (TPROTO_ISCSI == (bp[0] >> 4))) {
                                snprintf(b, b_len, "%.*s", sns_dlen, u_sns);
                                return b;
                        }
                }
        } else
                sns_dlen = 0;

        if (0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_LU,
                                    3 /* NAA */, 1 /* binary */)) {
                dlen = bp[off + 3];
                if (! ((8 == dlen) || (16 ==dlen)))
                        return b;
                if (want_prefix) {
                        if ((n = snprintf(cp, b_len, "naa.")) >= b_len)
                            n = b_len - 1;
                        cp += n;
                        b_len -= n;
                }
                for (k = 0; ((k < dlen) && (b_len > 1)); ++k) {
                        snprintf(cp, b_len, "%02x", bp[off + 4 + k]);
                        cp += 2;
                        b_len -= 2;
                }
        } else if (0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_LU,
                                           2 /* EUI */, 1 /* binary */)) {
                dlen = bp[off + 3];
                if (! ((8 == dlen) || (12 == dlen) || (16 ==dlen)))
                        return b;
                if (want_prefix) {
                        if ((n = snprintf(cp, b_len, "eui.")) >= b_len)
                            n = b_len - 1;
                        cp += n;
                        b_len -= n;
                }
                for (k = 0; ((k < dlen) && (b_len > 1)); ++k) {
                        snprintf(cp, b_len, "%02x", bp[off + 4 + k]);
                        cp += 2;
                        b_len -= 2;
                }
        } else if (0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_LU,
                                           0xa /* UUID */,  1 /* binary */)) {
                dlen = bp[off + 3];
                if ((1 != ((bp[off + 4] >> 4) & 0xf)) || (18 != dlen)) {
                        snprintf(cp, b_len, "??");
                        /* cp += 2; */
                        /* b_len -= 2; */
                } else {
                        if (want_prefix) {
                                if ((n = snprintf(cp, b_len, "uuid.")) >=
                                    b_len)
                                    n = b_len - 1;
                                cp += n;
                                b_len -= n;
                        }
                        for (k = 0; (k < 16) && (b_len > 1); ++k) {
                                if ((4 == k) || (6 == k) || (8 == k) ||
                                    (10 == k)) {
                                        snprintf(cp, b_len, "-");
                                        ++cp;
                                        --b_len;
                                }
                                snprintf(cp, b_len, "%02x",
                                         (unsigned int)bp[off + 6 + k]);
                                cp += 2;
                                b_len -= 2;
                        }
                }
        } else if (sns_dlen > 0)
                snprintf(b, b_len, "%.*s", sns_dlen, u_sns);
        else if ((0 == sg_vpd_dev_id_iter(bp, len, &off, VPD_ASSOC_LU,
                                          0x1 /* T10 vendor ID */,  -1)) &&
                 ((bp[off] & 0xf) > 1 /* ASCII or UTF */)) {
                dlen = bp[off + 3];
                if (dlen < 8)
                        return b;       /* must have 8 byte T10 vendor id */
                if (want_prefix) {
                        if ((n = snprintf(cp, b_len, "t10.")) >= b_len)
                            n = b_len - 1;
                        cp += n;
                        b_len -= n;
                }
                snprintf(cp, b_len, "%.*s", dlen, bp + off + 4);
        }
        return b;
}

/*  Parse colon_list into host/channel/target/lun ("hctl") array,
 *  return true if successful, else false.
 */
static bool
parse_colon_list(const char * colon_list, struct addr_hctl * outp)
{
        int k;
        unsigned short u;
        uint64_t z;
        const char * elem_end;

        if ((! colon_list) || (! outp))
                return false;
        if (1 != sscanf(colon_list, "%d", &outp->h))
                return false;
        if (NULL == (elem_end = strchr(colon_list, ':')))
                return false;
        colon_list = elem_end + 1;
        if (1 != sscanf(colon_list, "%d", &outp->c))
                return false;
        if (NULL == (elem_end = strchr(colon_list, ':')))
                return false;
        colon_list = elem_end + 1;
        if (1 != sscanf(colon_list, "%d", &outp->t))
                return false;
        if (NULL == (elem_end = strchr(colon_list, ':')))
                return false;
        colon_list = elem_end + 1;
        if (1 != sscanf(colon_list, "%" SCNu64 , &outp->l))
                return false;
        z = outp->l;
        for (k = 0; k < 4; ++k, z >>= 16) {
                u = z & 0xffff;
                outp->lun_arr[(2 * k) + 1] = u & 0xff;
                outp->lun_arr[2 * k] = (u >> 8) & 0xff;
        }
        return true;
}


/* Check host associated with 'devname' for known transport types. If so set
 * transport_id, place a string in 'b' and return true. Otherwise return
 * false. */
static bool
transport_init(const char * devname, /* const struct lsscsi_opts * op, */
               int b_len, char * b)
{
        int off;
        char * cp;
        char buff[LMAX_DEVPATH];
        char wd[LMAX_PATH];
        struct stat a_stat;



        /* SAS host */
        /* SAS transport layer representation */
        snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot, sas_host, devname);
        if ((stat(buff, &a_stat) >= 0) && S_ISDIR(a_stat.st_mode)) {
                transport_id = TRANSPORT_SAS;
                off = strlen(buff);
                snprintf(buff + off, sizeof(buff) - off, "/device");
                if (sas_low_phy_scan(buff, NULL) < 1)
                        return false;
                snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot, sas_phy,
                         sas_low_phy);
                snprintf(b, b_len, "sas:");
                off = strlen(b);
                if (get_value(buff, "sas_address", b + off, b_len - off))
                        return true;
                else
                        pr2serr("_init: no sas_address, wd=%s\n", buff);
        }

        /* SAS class representation */
        snprintf(buff, sizeof(buff), "%s%s%s%s", sysfsroot, scsi_host,
                 devname, "/device/sas/ha");
        if ((stat(buff, &a_stat) >= 0) && S_ISDIR(a_stat.st_mode)) {
                transport_id = TRANSPORT_SAS_CLASS;
                snprintf(b, b_len, "sas:");
                off = strlen(b);
                if (get_value(buff, "device_name", b + off, b_len - off))
                        return true;
                else
                        pr2serr("_init: no device_name, wd=%s\n", buff);
        }


        /* USB host? */
        cp = get_usb_devname(devname, NULL, wd, sizeof(wd) - 1);
        if (cp) {
                transport_id = TRANSPORT_USB;
                snprintf(b, b_len, "usb:%s", cp);
                return true;
        }

        /* ATA or SATA host, crude check: driver name */
        snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot, scsi_host, devname);
        if (get_value(buff, "proc_name", wd, sizeof(wd))) {
                if (0 == strcmp("ahci", wd)) {
                        transport_id = TRANSPORT_SATA;
                        snprintf(b, b_len, "sata:");
                        return true;
                } else if (strstr(wd, "ata")) {
                        if (0 == memcmp("sata", wd, 4)) {
                                transport_id = TRANSPORT_SATA;
                                snprintf(b, b_len, "sata:");
                                return true;
                        }
                        transport_id = TRANSPORT_ATA;
                        snprintf(b, b_len, "ata:");
                        return true;
                }
        }
        return false;
}




/* Attempt to determine the transport type of the SCSI device (LU) associated
 * with 'devname'. If found set transport_id, place string in 'b' and return
 * true. Otherwise return false. */
static bool
transport_tport(const char * devname, const struct lsscsi_opts * op,
                int b_len, char * b)
{
        bool ata_dev;
        int off;
        char * cp;
        char buff[LMAX_DEVPATH];
        char wd[LMAX_PATH];

        struct addr_hctl hctl;
        struct stat a_stat;

        if (! parse_colon_list(devname, &hctl))
                return false;

        /* check for SAS host */
        snprintf(buff, sizeof(buff), "%s%shost%d", sysfsroot, sas_host,
                 hctl.h);
        if ((stat(buff, &a_stat) >= 0) && S_ISDIR(a_stat.st_mode)) {
                /* SAS transport layer representation */
                transport_id = TRANSPORT_SAS;
                snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot,
                         class_scsi_dev, devname);
                if (if_directory_chdir(buff, "device")) {
                        if (NULL == getcwd(wd, sizeof(wd)))
                                return false;
                        cp = strrchr(wd, '/');
                        if (NULL == cp)
                                return false;
                        *cp = '\0';
                        cp = strrchr(wd, '/');
                        if (NULL == cp)
                                return false;
                        *cp = '\0';
                        cp = basename(wd);
                        my_strcopy(sas_hold_end_device, cp,
                                   sizeof(sas_hold_end_device));
                        snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot,
                                 sas_device, cp);

                        snprintf(b, b_len, "sas:");
                        off = strlen(b);
                        if (get_value(buff, "sas_address", b + off,
                                      b_len - off))
                                return true;
                        else {  /* non-SAS device in SAS domain */
                                snprintf(b + off, b_len - off,
                                         "0x0000000000000000");
                                if (op->verbose > 1)
                                        pr2serr("%s: no sas_address, wd=%s\n",
                                                __func__, buff);
                                return true;
                        }
                } else
                        pr2serr("%s: down FAILED: %s\n", __func__, buff);
                return false;
        }

        /* SAS class representation or SBP? */
        snprintf(buff, sizeof(buff), "%s%s/%s", sysfsroot, bus_scsi_devs,
                 devname);
        if (if_directory_chdir(buff, "sas_device")) {
                transport_id = TRANSPORT_SAS_CLASS;
                snprintf(b, b_len, "sas:");
                off = strlen(b);
                if (get_value(".", "sas_addr", b + off, b_len - off))
                        return true;
                else
                        pr2serr("%s: no sas_addr, wd=%s\n", __func__, buff);
        } else if (get_value(buff, "ieee1394_id", wd, sizeof(wd))) {
                /* IEEE1394 SBP device */
                transport_id = TRANSPORT_SBP;
                snprintf(b, b_len, "sbp:%s", wd);
                return true;
        }


        /* USB device? */
        cp = get_usb_devname(NULL, devname, wd, sizeof(wd) - 1);
        if (cp) {
                transport_id = TRANSPORT_USB;
                snprintf(b, b_len, "usb:%s", cp);
                return true;
        }

        /* ATA or SATA device, crude check: driver name */
        snprintf(buff, sizeof(buff), "%s%shost%d", sysfsroot, scsi_host,
                 hctl.h);
        if (get_value(buff, "proc_name", wd, sizeof(wd))) {
                ata_dev = false;
                if (0 == strcmp("ahci", wd)) {
                        transport_id = TRANSPORT_SATA;
                        snprintf(b, b_len, "sata:");
                        ata_dev = true;
                } else if (strstr(wd, "ata")) {
                        if (0 == memcmp("sata", wd, 4)) {
                                transport_id = TRANSPORT_SATA;
                                snprintf(b, b_len, "sata:");
                        } else {
                                transport_id = TRANSPORT_ATA;
                                snprintf(b, b_len, "ata:");
                        }
                        ata_dev = true;
                }
                if (ata_dev) {
                        off = strlen(b);
                        snprintf(b + off, b_len - off, "%s",
                                 get_lu_name(devname, wd, sizeof(wd), false));
                        return true;
                }
        }
        return false;
}

/* Given the transport_id of the SCSI device (LU) associated with 'devname'
 * output additional information. */
static void
transport_tport_mini(const char * devname, const struct lsscsi_opts * op, struct disk_entry *e)
{
        char path_name[LMAX_DEVPATH];
        char buff[LMAX_DEVPATH];
        char b2[LMAX_DEVPATH];
        char value[LMAX_NAME];


#if 0
        snprintf(buff, sizeof(buff), "%s/scsi_device:%s", path_name, devname);
        if (! if_directory_chdir(buff, "device"))
                return;
        if (NULL == getcwd(wd, sizeof(wd)))
                return;
#else
        snprintf(path_name, sizeof(path_name), "%s%s%s", sysfsroot,
                 class_scsi_dev, devname);
        my_strcopy(buff, path_name, sizeof(buff));
#endif
        switch (transport_id) {

        case TRANSPORT_SAS:

                snprintf(buff, sizeof(buff), "%s%s%s", sysfsroot, sas_device,
                         sas_hold_end_device);


                snprintf(b2, sizeof(b2), "%s%s%s", sysfsroot, sas_end_device,
                         sas_hold_end_device);

                if (get_value(buff, "bay_identifier", value, sizeof(value))) {
                	//printf("bay=%s ", value);
                	// FIXME LSI 2008 mpt2sas may not have information, 10.4.16.14
                	// 10.3.21.14 SAS3008 PCI-Express Fusion-MPT SAS-3 (rev 02) also null
                	if(value[0]) {
                		e->slot = atoi(value);
                	}
                }

                if (get_value(buff, "enclosure_identifier", value,
                              sizeof(value))) {
                	if(value[0]) {
						//printf("ec=%s ", value + 2);
						e->ec = get_ec_id(value + 2);
						snprintf(e->ec_addr, sizeof(e->ec_addr), "%s", value + 2);
                	} else {
                		// LSI 2008 mpt2sas may not have information, 10.4.16.14
                		// keep NA;
                	}
                }

                if (get_value(buff, "phy_identifier", value, sizeof(value))) {
                    //printf("phy=%s ", value);
                	if(value[0] && e->slot < 0) {
                		// case of 10.3.21.14 SAS3008 PCI-Express Fusion-MPT SAS-3 (rev 02) also null
                		e->slot = atoi(value);
                	}
                }


                break;
        case TRANSPORT_SAS_CLASS:
        		//printf("NO_INFO_SASC | ");
                break;
        case TRANSPORT_ATA:
        		//printf("NO_INFO_ATA | ");
                break;
        case TRANSPORT_SATA:
                //printf("NO_INFO_SATA | ");
                break;
        default:
        		//printf("NO_INFO | ");
                break;
        }
}




static void
tag_lun_helper(int * tag_arr, int kk, int num)
{
        int j;

        for (j = 0; j < num; ++j)
                tag_arr[(2 * kk) + j] = ((kk > 0) && (0 == j)) ? 2 : 1;
}

/* Tag lun bytes according to SAM-5 rev 10. Write output to tag_arr assumed
 * to have at least 8 ints. 0 in tag_arr means this position and higher can
 * be ignored; 1 means print as is; 2 means print with separator
 * prefixed. Example: lunp: 01 22 00 33 00 00 00 00 generates tag_arr
 * of 1, 1, 2, 1, 0 ... 0 and might be printed as 0x0122_0033 . */
void
tag_lun(const unsigned char * lunp, int * tag_arr)
{
        bool next_level;
        int k, a_method, bus_id, len_fld, e_a_method;
        unsigned char not_spec[2] = {0xff, 0xff};

        if (NULL == tag_arr)
                return;
        for (k = 0; k < 8; ++k)
                tag_arr[k] = 0;
        if (NULL == lunp)
                return;
        if (0 == memcmp(lunp, not_spec, sizeof(not_spec))) {
                for (k = 0; k < 2; ++k)
                        tag_arr[k] = 1;
                return;
        }
        for (k = 0; k < 4; ++k, lunp += 2) {
                next_level = false;
                a_method = (lunp[0] >> 6) & 0x3;
                switch (a_method) {
                case 0:         /* peripheral device addressing method */
                        bus_id = lunp[0] & 0x3f;
                        if (bus_id)
                            next_level = true;
                        tag_lun_helper(tag_arr, k, 2);
                        break;
                case 1:         /* flat space addressing method */
                        tag_lun_helper(tag_arr, k, 2);
                        break;
                case 2:         /* logical unit addressing method */
                        tag_lun_helper(tag_arr, k, 2);
                        break;
                case 3:         /* extended logical unit addressing method */
                        len_fld = (lunp[0] & 0x30) >> 4;
                        e_a_method = lunp[0] & 0xf;
                        if ((0 == len_fld) && (1 == e_a_method))
                                tag_lun_helper(tag_arr, k, 2);
                        else if ((1 == len_fld) && (2 == e_a_method))
                                tag_lun_helper(tag_arr, k, 4);
                        else if ((2 == len_fld) && (2 == e_a_method))
                                tag_lun_helper(tag_arr, k, 6);
                        else if ((3 == len_fld) && (0xf == e_a_method))
                                tag_arr[2 * k] = (k > 0) ? 2 : 1;
                        else {
                                if (len_fld < 2)
                                        tag_lun_helper(tag_arr, k, 4);
                                else {
                                        tag_lun_helper(tag_arr, k, 6);
                                        if (3 == len_fld) {
                                                tag_arr[(2 * k) + 6] = 1;
                                                tag_arr[(2 * k) + 7] = 1;
                                        }
                                }
                        }
                        break;
                default:
                        tag_lun_helper(tag_arr, k, 2);
                        break;
                }
                if (! next_level)
                        break;
        }
}

static uint64_t
lun_word_flip(uint64_t in)
{
        int k;
        uint64_t res = 0;

        for (k = 0; ; ++k) {
                res |= (in & 0xffff);
                if (k > 2)
                        break;
                res <<= 16;
                in >>= 16;
        }
        return res;
}


void fill_disk_type(struct disk_entry *e)
{
	char fname[64];
	char buf[64];

	// nvme type
	if(e->path[5] == 'n' && e->path[6] == 'v' ) {
		strcpy(e->disk_type, "NVME SSD");
		return;
	}

	// already set by vpdb1
    if(e->disk_type[0] == 'H' || e->disk_type[0] == 'S') {
        return;
    }

	sprintf(buf, "%s", e->path + 5);
	sprintf(fname, "/sys/block/%s/queue/rotational", buf);
	//printf("%s ***", fname);
	FILE *fp = fopen(fname, "r");
	if(fp == NULL) {
		// for nvme
		return;
	}
	fgets(buf, 8, fp);
	if (buf[0] == '0') {
		strcpy(e->disk_type, "SSD");
	}
	if (buf[0] == '1') {
		strcpy(e->disk_type, "HDD");
	}
	fclose(fp);
}

/* List one SCSI device (LU) on a line. */
static void
one_sdev_entry(const char * dir_name, const char * devname,
               const struct lsscsi_opts * op)
{
	int type, n, vlen;
	char buff[LMAX_DEVPATH];
	char extra[LMAX_DEVPATH];
	char value[LMAX_NAME];
	char wd[LMAX_PATH];
	char scsi_name[16] = {'\0', };
	char sg_name[16] = {'\0', };
	struct addr_hctl hctl;
	int vdp89_ret = 1;

	struct disk_entry *e = sys_disk_depot + sys_disk_idx;
	init_disk_entry(e);

	vlen = sizeof(value);
	snprintf(buff, sizeof(buff), "%s/%s", dir_name, devname);
	// op->lunhex

	if (parse_colon_list(devname, &hctl)) {

			e->host = hctl.h;
			e->bus = hctl.c;
			e->target = hctl.t;
			e->lun = lun_word_flip(hctl.l);

	} else {
			snprintf(value, vlen, "[%s]", devname);
	}


	if (! get_value(buff, "type", value, vlen)) {
			//printf("KNOW_TYPE1 | ");
	} else if (1 != sscanf(value, "%d", &type)) {
			//printf("KNOW_TYPE1 | ");
	} else if ((type < 0) || (type > 31)) {
			//printf("KNOW_TYPE3 | ");
	} else {
			snprintf(e->pd_type, 16, "%s", scsi_short_device_types[type]);
	}



	// transport_tport, eg sata:5002538c40192375
	//op->transport_info
	if (transport_tport(devname, op, vlen, value)) {

		// SAS HBA mode
		if(value[0] == 's' && value[1] == 'a' && value[2] == 's') {
			strncpy(e->peer_addr, value + 6, 64);
			sprintf(e->connect_type, "SAS_HBA");
		}

		// PCH mode
		if(value[0] == 's' && value[1] == 'a' && value[2] == 't') {
			e->ec = 0;
			e->slot = e->host;
			sprintf(e->connect_type, "SATA_HBA");
		}

		// JBOD mode no information
	}

	transport_tport_mini(devname, op, e);

	// WWN INFO
	get_lu_name(devname, value, vlen, op->unit > 3);
	n = strlen(value);
	if (n < 1)      /* left justified "none" means no lu name */
	{
			//printf(" NO_LU_WWN_INFO | ");
	} else     /* -uuu, output in full, append rest of line */
	{
			//printf("%s | ", value);
		sprintf(e->wwn, "%s", value);
	}


	if (1 == non_sg_scan(buff, op)) {
			if (DT_DIR == non_sg.d_type) {
					snprintf(wd, sizeof(wd), "%s/%s", buff, non_sg.name);
					// WD: /sys/bus/scsi/devices/0:0:0:0/block
					if (1 == scan_for_first(wd, op))
							my_strcopy(extra, aa_first.name,
									   sizeof(extra));
					else {
							printf("unexpected scan_for_first error");
							wd[0] = '\0';
					}
			} else {
					my_strcopy(wd, buff, sizeof(wd));
					my_strcopy(extra, non_sg.name, sizeof(extra));
			}
			if (wd[0] && (if_directory_chdir(wd, extra))) {
					if (NULL == getcwd(wd, sizeof(wd))) {
							printf("getcwd error");
							wd[0] = '\0';
					}
			}
			if (wd[0]) {
				// op->kname
				snprintf(scsi_name, 16, "%s/%s", dev_dir, basename(wd));
				snprintf(e->path, 16, "%s/%s", dev_dir, basename(wd));
			}
	}

	// op->generic
	//printf(" *%s* ", buff);
	if (if_directory_ch2generic(buff)) {
			if (NULL == getcwd(wd, sizeof(wd))) {
					//printf("  generic_dev error");
			}
			else {
				snprintf(sg_name, 16, "%s/%s", dev_dir, basename(wd));
				snprintf(e->sgpath, 16, "%s/%s", dev_dir, basename(wd));

			}
	}

	if(sg_name[0]) {
		vdp89_ret = vpd89(sg_name, e);
		vpdb1(sg_name, e);
	} else if(scsi_name[0]) {
		vdp89_ret = vpd89(scsi_name, e);
		vpdb1(scsi_name, e);
	}

	if(vdp89_ret) {

		// e->SN, keeps NA

		// version
		if (get_value(buff, "rev", value, vlen)) {
				//printf("%s | ", value);
			sprintf(e->VER, value);
		}


		// MODE
		if (get_value(buff, "model", value, vlen)) {
			//printf("%s | ", value);
			sprintf(e->MODEL, value);
		}

		// SAS_HBA_RAID SN handling
		if(sg_name[0]) {
			vpd80(sg_name, e);
			// ignore error ret
		}

	}

	print_host_pci_path(buff, e);

	char blkdir[LMAX_DEVPATH];


	my_strcopy(blkdir, buff, sizeof(blkdir));

	value[0] = 0;
	if (type == 0 &&
		block_scan(blkdir) &&
		if_directory_chdir(blkdir, ".") &&
		get_value(".", "size", value, vlen)) {
			uint64_t blocks = atoll(value);

			blocks <<= 9;
			if (blocks > 0) {
				e->sizeGB = (float)(blocks / 1000 / 1000 / 1000);
			}

	}

}

static int
sdev_dir_scan_select(const struct dirent * s)
{
/* Following no longer needed but leave for early lk 2.6 series */
        if (strstr(s->d_name, "mt"))
                return 0;       /* st auxiliary device names */
        if (strstr(s->d_name, "ot"))
                return 0;       /* osst auxiliary device names */
        if (strstr(s->d_name, "gen"))
                return 0;
/* Above no longer needed but leave for early lk 2.6 series */
        if (!strncmp(s->d_name, "host", 4)) /* SCSI host */
                return 0;
        if (!strncmp(s->d_name, "target", 6)) /* SCSI target */
                return 0;
        if (strchr(s->d_name, ':')) {
                if (filter_active) {
                        struct addr_hctl s_hctl;

                        if (! parse_colon_list(s->d_name, &s_hctl)) {
                                pr2serr("%s: parse failed\n", __func__);
                                return 0;
                        }
                        if (((-1 == filter.h) || (s_hctl.h == filter.h)) &&
                            ((-1 == filter.c) || (s_hctl.c == filter.c)) &&
                            ((-1 == filter.t) || (s_hctl.t == filter.t)) &&
                            (((uint64_t)~0 == filter.l) ||
                             (s_hctl.l == filter.l)))
                                return 1;
                        else
                                return 0;
                } else
                        return 1;
        }
        /* Still need to filter out "." and ".." */
        return 0;
}

/* Returns -1 if (a->d_name < b->d_name) ; 0 if they are equal
 * and 1 otherwise.
 * Function signature was more generic before version 0.23 :
 * static int sdev_scandir_sort(const void * a, const void * b)
 */
static int
sdev_scandir_sort(const struct dirent ** a, const struct dirent ** b)
{
        const char * lnam = (*a)->d_name;
        const char * rnam = (*b)->d_name;
        struct addr_hctl left_hctl;
        struct addr_hctl right_hctl;

        if (! parse_colon_list(lnam, &left_hctl)) {
                pr2serr("%s: left parse failed\n", __func__);
                return -1;
        }
        if (! parse_colon_list(rnam, &right_hctl)) {
                pr2serr("%s: right parse failed\n", __func__);
                return 1;
        }
        return cmp_hctl(&left_hctl, &right_hctl);
}


/* List SCSI devices (LUs). */
static void
list_sdevices(const struct lsscsi_opts * op)
{
	int num, k, p;
	struct dirent ** namelist;
	char buff[LMAX_DEVPATH];
	char name[LMAX_NAME];

	snprintf(buff, sizeof(buff), "%s%s", sysfsroot, bus_scsi_devs);

	num = scandir(buff, &namelist, sdev_dir_scan_select,
				  sdev_scandir_sort);
	if (num < 0) {  /* scsi mid level may not be loaded */
			if (op->verbose > 1) {
					snprintf(name, sizeof(name), "scandir: %s", buff);
					perror(name);
					printf("SCSI mid level module may not be loaded\n");
			}
			if (op->classic)
					printf("Attached devices: none\n");
			return;
	}

	// ld go first
	mega_ld_list();
	// besides ld, others are jbod and spare disks include ungood
	mega_jbod_list();

	for (k = 0; k < num; ++k) {
			my_strcopy(name, namelist[k]->d_name, sizeof(name));
			transport_id = TRANSPORT_UNKNOWN;
			one_sdev_entry(buff, name, op);
			sys_disk_idx++;
			free(namelist[k]);
	}

	free(namelist);

	scan_nvme_ns();

	if(op->verbose > 0) {
		for(k = 0; k < raid_ld_pd_idx; k++) {
				struct disk_entry *e = NULL;
				e = raid_pd_depot + k;
				dump_disk_entry(e);
		}

		for(k = 0; k < sys_disk_idx; k++) {
				struct disk_entry *e = NULL;
				e = sys_disk_depot + k;
				dump_disk_entry(e);
		}

		printf("=================================\n");
	}


	struct disk_entry *dst = NULL;
	int tmp_megasas_raid_pd_idx = 0;
	// now we do match
	for(k = 0; k < sys_disk_idx; k++) {
		struct disk_entry *src = NULL;
		src = sys_disk_depot + k;
		dst = final_depot + final_disk_idx;


		// it is the enclosure, or unpluged usb, or?
		if(src->sizeGB < 0 && src->pd_type[0] != 'd') {
			continue;
		}

		// unplugged usb, with path left
		if(src->sizeGB < 0 && src->pd_type[0] == 'd' && src->path[0] == '/') {
			continue;
		}

		// mpt3sas raid disk
		if(src->sizeGB < 0 && src->pd_type[0] == 'd') {
			memcpy(dst, src, sizeof(struct disk_entry));

			// very silly processing TODO
			for(p = 0; p < sys_disk_idx; p++) {
				//eg 600508e000000000abe63b8579dabc05
				if(strlen(sys_disk_depot[p].wwn) >= 32) {
					strcpy(dst->path, sys_disk_depot[p].path);
					// FIXME bug here, only raid1 is correct
					dst->sizeGB = sys_disk_depot[p].sizeGB;
					break;
				}
			}
			sprintf(dst->connect_type, "SAS_HBA_RAID");
			fill_disk_type(dst);
			// TODO, SN and SSD TYPE *** n6-193-013
			dst->vd_idx = 0;
			dst->pd_idx = tmp_megasas_raid_pd_idx;
			//printf("%f\n", dst->sizeGB);
			readCap(dst->sgpath, dst);
			//printf("%f\n", dst->sizeGB);
			// FIXME bug here, only 1 raid is support
			tmp_megasas_raid_pd_idx++;

			sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai %s", dst->sgpath);
			final_disk_idx++;
			continue;
		}

		// megasas raid disk
		if(strlen(src->wwn) >= 32) {
			for(p = 0; p < raid_ld_pd_idx; p++) {
				if(raid_pd_depot[p].vd_idx == src->target) {
					// final_disk_idx may updated
					dst = final_depot + final_disk_idx;
					memcpy(dst, raid_pd_depot + p, sizeof(struct disk_entry));
					strcpy(dst->path, src->path);
					strcpy(dst->sgpath, src->sgpath);
					strcpy(dst->pcipath, src->pcipath);
					// MODEL got from OS, it is raid's model, such as PERC
					// strcpy(dst->MODEL, src->MODEL);
					dst->host = src->host;
					dst->bus = src->bus;
					dst->target = src->target;
					dst->lun = src->lun;
					sprintf(dst->connect_type, "RAID_RAID");
					if(raid_pd_depot[p].pd_type[0] == 'H') {
						strcpy(dst->disk_type, "HDD");
					}
					if(raid_pd_depot[p].pd_type[0] == 'S') {
						strcpy(dst->disk_type, "SSD");
					}

					if((dst->did > -1) && (dst->did != dst->slot)) {
						// printf("slot %d, did %d\n", dst->slot, dst->did);
						sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai -d megaraid,%d /dev", dst->did);
					} else {
						sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai -d megaraid,%d /dev", dst->slot);
					}
					final_disk_idx++;
				}
			}
			// end match with src
			continue;
		}

		// megasas jbod disk, NVME, and HBA remainning (all the non raid disk)
		// maybe still bug TODO FIXME
		// megasas jbod disk, exclude nvme, but no NA
		if(src->ec < 0 && src->slot < 0 && src->disk_type[1] != 'V') {
			for(p = 0; p < raid_ld_pd_idx; p++) {
				// different with raid pd, which is the vd_idx, lsi jbod use slot idx
				if(raid_pd_depot[p].slot == src->target) {
					// final_disk_idx may updated
					dst = final_depot + final_disk_idx;
					memcpy(dst, raid_pd_depot + p, sizeof(struct disk_entry));
					strcpy(dst->path, src->path);
					strcpy(dst->sgpath, src->sgpath);
					strcpy(dst->pcipath, src->pcipath);
					// MODEL got from OS, SN NA
					strcpy(dst->MODEL, src->MODEL);
					strcpy(dst->VER, src->VER);
					strcpy(dst->SN, src->SN);
					dst->host = src->host;
					dst->bus = src->bus;
					dst->target = src->target;
					dst->lun = src->lun;

					dst->vd_idx = 999;
					dst->pd_idx = 0;

					sprintf(dst->connect_type, "RAID_JBOD");
					fill_disk_type(dst);

					sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai -d megaraid,%d /dev", dst->slot);
					final_disk_idx++;
					break;
				}
			}
			continue;
		}


		// normal disk got here
		memcpy(dst, src, sizeof(struct disk_entry));
		// SAS_HBA or SATA_HBA or NVME
		fill_disk_type(dst);
		if(dst->sgpath[0] == '/') {
			sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai %s", dst->sgpath);
		} else {
			sprintf(smartcmd[final_disk_idx], "/etc/sysop/healthd2/bin/smartctl -Ai %s", dst->path);
			if(strcmp(dst->disk_type, "NVME SSD") == 0){
				// FIXME, multiple ns
				fix_nvme_ns_smartcmd(smartcmd[final_disk_idx]);
			}
		}
		final_disk_idx++;
	}

	for(k = 0; k < final_disk_idx; k++) {
			struct disk_entry *e = NULL;
			e = final_depot + k;
			//dump_disk_entry(e);
			//printf("%s\n", smartcmd[k]);
	}

}

#define SMART_DM1 "Device Model:"
// non ata inq
#define SMART_DM2 "Product:"
// nvme
#define SMART_DM3 "Model Number:"
#define SMART_SN1 "Serial Number:"
#define SMART_SN2 "Serial number:"

int mkpath(char* file_path, mode_t mode) {
	if(file_path == NULL || *file_path == '\0') {
		return -1;
	}

	char* p;
	for (p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = '\0';
		if (mkdir(file_path, mode) == -1) {
			if (errno != EEXIST) {
				*p = '/';
				return -1;
			}
		}
		*p = '/';
	}
	return 0;
}

void periodic(char *buf, int max_len, char *smart_log_path) {
	int k;
	for(k = 0; k < final_disk_idx; k++) {
		char cmd[256];
		char smartlog_path[256];
		char DM[64] = {'\0', };
		char SN[64] = {'\0', };
		char path[16] = {'\0', };
		char *p1 = buf, *p2 = NULL;
		int len;
		int fsize;

		sprintf(path, "%s", basename(final_depot[k].path));
		sprintf(cmd, "%s", smartcmd[k]);
		FILE *fp = NULL;
		fp = popen(cmd, "r");
		if(fp) {
			while((p1 - buf + 1024) < max_len && (p2 = fgets(p1, 1024, fp)) != NULL) {
				len = strlen(p2);
				//printf("Got len %d %s", len, p2);
				p1 += len;
				// next time will override '\0'

				if(strncmp(p2, SMART_DM1, strlen(SMART_DM1)) == 0) {
					p2 += strlen(SMART_DM1);
					while(*p2 == ' ') {
						p2++;
					}
					len = strlen(p2);
					snprintf(DM, len, "%s", p2);
				}

				if(strncmp(p2, SMART_DM2, strlen(SMART_DM2)) == 0) {
					p2 += strlen(SMART_DM2);
					while(*p2 == ' ') {
						p2++;
					}
					len = strlen(p2);
					snprintf(DM, len, "%s", p2);
				}

				if(strncmp(p2, SMART_DM3, strlen(SMART_DM3)) == 0) {
					p2 += strlen(SMART_DM3);
					while(*p2 == ' ') {
						p2++;
					}
					len = strlen(p2);
					snprintf(DM, len, "%s", p2);
				}

				if(strncmp(p2, SMART_SN1, strlen(SMART_SN1)) == 0) {
					p2 += strlen(SMART_SN1);
					while(*p2 == ' ') {
						p2++;
					}
					len = strlen(p2);
					snprintf(SN, len, "%s", p2);
				}

				if(strncmp(p2, SMART_SN2, strlen(SMART_SN2)) == 0) {
					p2 += strlen(SMART_SN2);
					while(*p2 == ' ') {
						p2++;
					}
					len = strlen(p2);
					snprintf(SN, len, "%s", p2);
				}
			}
			pclose(fp);
			*p1 = '\0';
		}
		fsize = ((p1 - buf) > 0) ? (p1 - buf - 1) : 0;


		len = 0;
		if(fsize) {
			snprintf(smartlog_path, 255, "%s/%s.%s.%s.log", smart_log_path, DM, SN, path);
			mkpath(smartlog_path, 0600);
			fp = fopen(smartlog_path, "w");
			if(fp) {
				len = fwrite(buf, fsize, 1, fp);
				// TODO ERROR detection
				fclose(fp);
			}
		}

		//printf("Size %d %s %s %s %p %p\n", fsize, DM, SN, path, p1, buf);
		//printf("%s", buf);

		if(len) {
			sprintf(cmd, "/bin/cat '%s' | /etc/sysop/healthd2/bin/nc -p %d -q 0 -w %d %s %d",
					smartlog_path, LOCAL_PORT, NET_TIMEOUT_SEC, REMOTE_SRV, REMOTE_PORT);
			fp = popen(cmd, "r");
			if(fp) {
				// pclose will call wait4
				pclose(fp);
				sleep(1);
			}
		}

	}
}


static void
one_host_entry(const char * dir_name, const char * devname,
               const struct lsscsi_opts * op)
{
        unsigned int host_id;
        const char * nullname1 = "<NULL>";
        const char * nullname2 = "(null)";
        char buff[LMAX_DEVPATH];
        char value[LMAX_NAME];
        char wd[LMAX_PATH];

        if (op->classic) {
                // one_classic_host_entry(dir_name, devname, op);
                printf("  <'--classic' not supported for hosts>\n");
                return;
        }
        if (1 == sscanf(devname, "host%u", &host_id))
                printf("[%u]  ", host_id);
        else
                printf("[?]  ");
        snprintf(buff, sizeof(buff), "%s/%s", dir_name, devname);
        if ((get_value(buff, "proc_name", value, sizeof(value))) &&
            (strncmp(value, nullname1, 6)) && (strncmp(value, nullname2, 6)))
                printf("  %-12s  ", value);
        else if (if_directory_chdir(buff, "device/../driver")) {
                if (NULL == getcwd(wd, sizeof(wd)))
                        printf("  %-12s  ", nullname2);
                else
                        printf("  %-12s  ", basename(wd));

        } else
                printf("  proc_name=????  ");
        // op->transport_info)

		if (transport_init(devname, /* op, */ sizeof(value), value))
				printf("%s\n", value);
		else
				printf("\n");


        if (op->verbose > 0) {
                printf("  dir: %s\n  device dir: ", buff);
                if (if_directory_chdir(buff, "device")) {
                        if (NULL == getcwd(wd, sizeof(wd)))
                                printf("?");
                        else
                                printf("%s", wd);
                }
                printf("\n");
        }
}

static int
host_dir_scan_select(const struct dirent * s)
{
        int h;

        if (0 == strncmp("host", s->d_name, 4)) {
                if (filter_active) {
                        if (-1 == filter.h)
                                return 1;
                        else if ((1 == sscanf(s->d_name + 4, "%d", &h) &&
                                 (h == filter.h)))
                                return 1;
                        else
                                return 0;
                } else
                        return 1;
        }
        return 0;
}

/* Returns -1 if (a->d_name < b->d_name) ; 0 if they are equal
 * and 1 otherwise.
 * Function signature was more generic before version 0.23 :
 * static int host_scandir_sort(const void * a, const void * b)
 */
static int
host_scandir_sort(const struct dirent ** a, const struct dirent ** b)
{
        unsigned int l, r;
        const char * lnam = (*a)->d_name;
        const char * rnam = (*b)->d_name;

        if (1 != sscanf(lnam, "host%u", &l))
                return -1;
        if (1 != sscanf(rnam, "host%u", &r))
                return 1;
        if (l < r)
                return -1;
        else if (r < l)
                return 1;
        return 0;
}

static void
list_hosts(const struct lsscsi_opts * op)
{
        int num, k;
        struct dirent ** namelist;
        char buff[LMAX_DEVPATH];
        char name[LMAX_NAME];

        snprintf(buff, sizeof(buff), "%s%s", sysfsroot, scsi_host);

        num = scandir(buff, &namelist, host_dir_scan_select,
                      host_scandir_sort);
        if (num < 0) {
                snprintf(name, sizeof(name), "scandir: %s", buff);
                perror(name);
                return;
        }
        if (op->classic)
                printf("Attached hosts: %s\n", (num ? "" : "none"));

        for (k = 0; k < num; ++k) {
                my_strcopy(name, namelist[k]->d_name, sizeof(name));
                transport_id = TRANSPORT_UNKNOWN;
                one_host_entry(buff, name, op);
                free(namelist[k]);
        }
        free(namelist);
}


int write_list_to_file(FILE *fp) {
	struct disk_entry *e = NULL;
	time_t cur_time;
	struct tm cur_tminfo;
	int k = 0;
	char date_str[32];

	time(&cur_time);
	localtime_r(&cur_time, &cur_tminfo);
	strftime(date_str, 32, "%Y-%m-%d %H:%M:%S", &cur_tminfo);

	fprintf(fp, "\n");
	fprintf(fp, "=== DISK LIST ### %s ### VER %s ===\n", date_str, VER_STR);
	for(k = 0; k < final_disk_idx; k++) {
		e = final_depot + k;
		fprintf(fp, "%s | %s | ", e->sgpath, e->path);
		fprintf(fp, "%d | %d | ", e->ec, e->slot);
		fprintf(fp, "%d | %d | ", e->vd_idx, e->pd_idx);
		fprintf(fp, "%s | %s | %s | ", e->wwn, e->peer_addr, e->ec_addr);
		fprintf(fp, "%s | %s | %s | %s | ", e->pcipath, e->pd_type, e->disk_type, e->connect_type);
		fprintf(fp, "%d | %d | %d | %d | ", e->host, e->bus, e->target,  e->lun);
		fprintf(fp, "%.0f | %s | ", e->sizeGB, e->raid_inq);
		fprintf(fp, "%s | %s | %s \n", e->MODEL, e->VER, e->SN);
	}
	fprintf(fp, "\n");
	fflush(fp);
}


int gen_list(char *log_path)
{
	struct lsscsi_opts * op;
	struct lsscsi_opts opts;

	op = &opts;
	memset(op, 0, sizeof(opts));

	op->verbose = 0;
	op->transport_info = true;

	sys_disk_idx = 0;
	raid_ld_pd_idx = 0;
	final_disk_idx = 0;

	cur_ec[0] = '\0';
	last_ec[0] = '\0';
	ec_idx = -1;

	//list_hosts(op);
	list_sdevices(op);
	free_dev_node_list();

	char fpath[256];
	snprintf(fpath, 255, "%s/disklist.txt", log_path);
	mkpath(fpath, 0600);
	FILE *fp = fopen(fpath, "w");
	if(fp) {
		write_list_to_file(fp);
		fclose(fp);
	}

	char cmd[256];
	sprintf(cmd, "/bin/cat '%s' | /etc/sysop/healthd2/bin/nc -p %d -q 0 -w %d %s %d",
			fpath, LOCAL_PORT, NET_TIMEOUT_SEC, REMOTE_SRV, REMOTE_PORT);
	fp = popen(cmd, "r");
	if(fp) {
		// pclose will call wait4
		pclose(fp);
	}

	return 0;
}


#ifdef DISK_LIST_STD_APP

int main(int argc, char *argv[]) {

	struct lsscsi_opts * op;
	struct lsscsi_opts opts;

	op = &opts;
	memset(op, 0, sizeof(opts));

	op->verbose = 0;
	op->transport_info = true;


	if(argc > 1) {
		op->verbose = 1;
	}

	sys_disk_idx = 0;
	raid_ld_pd_idx = 0;
	final_disk_idx = 0;

	cur_ec[0] = '\0';
	last_ec[0] = '\0';
	ec_idx = -1;

	//list_hosts(op);
	list_sdevices(op);
	free_dev_node_list();

	write_list_to_file(stdout);

	if(op->verbose > 0) {
		int k;

		for(k = 0; k < final_disk_idx; k++) {
			printf("%d: %s\n", k, smartcmd[k]);
		}
		printf("\n");
	}

	return 0;
}

#endif
