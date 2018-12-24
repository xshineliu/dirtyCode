
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>

#define EDAC_MC_DIR "/sys/devices/system/edac/mc/"
#define MC_STR "mc"
#define CSROW_STR "csrow"
#define CHANNEL_STR "ch"

#define DMI_ENTRY_DIR "/sys/firmware/dmi/entries/"
#define DMI_TYPE_DIMM_STR "17-"

///////////////////////////////////////////////

// THIS IS THE LIMATAION
#define MAX_PKG 8
#define MAX_CORE 64
#define MAX_HT 2

#define PRODUCT_NAME_FILE "/sys/devices/virtual/dmi/id/product_name"
#define BOARD_VENDOR_FILE "/sys/devices/virtual/dmi/id/board_vendor"

#define PKG_ID_FILE "/sys/devices/system/cpu/cpu%d/topology/physical_package_id"
#define CORE_ID_FILE "/sys/devices/system/cpu/cpu%d/topology/core_id"
#define HT_ID_FILE "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list"
#define CPU_LIST "/dev/cpu/"

#define MAX_DIMM_SLOT 32

#define __cpuid(level, a, b, c, d)			\
  __asm__ ("cpuid\n\t"					\
	   : "=a" (a), "=b" (b), "=c" (c), "=d" (d)	\
	   : "0" (level))


struct dimm_info{
	// slot seq from 1
	short seq;
	short sz_gb;
	int topo;
	int edac_path;
	int edac_val;
	char dloc[64];
	char bloc[64];
	char pn[64];
	char sn[64];
};

struct dimm_info dimm_info_data[MAX_DIMM_SLOT];

short topo[MAX_PKG][MAX_CORE][MAX_HT];
int nr_pkg = -1;
int core_id_max = -1;
int core_per_pkg = 0;
int ht_per_core = -1;
int nr_lcpu = 0;

int dimm_plugged = 0;
int nr_dimm_slots = 0;

unsigned int family;
unsigned int model;

char pname[64];
char bvname[64];

char get_ht_index(int cpu_idx) {
	char filename[128];
	int fd;
	// FIXME
	char data[8];
	sprintf(filename, HT_ID_FILE, cpu_idx);
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	if (pread(fd, (void *)&data, sizeof data, 0) < 1) {
		close(fd);
		return -1;
	}
	char ret = -1;
	// cat  /sys/devices/system/cpu/cpu39/topology/thread_siblings_list | hexdump -C
	// 00000000  31 39 2c 33 39 0a                                 |19,39.|
	int n = strlen(data) - 1;
	data[n] = '\0';
	int idx1 = 0;
	int idx2 = 0;
	//arch_cpu_data.dbg(LOG_NOTICE, "%s, got PRE %d %d %d\n", NAME, n, idx1, idx2);
	while(idx2 < (n + 1)) {
		//arch_cpu_data.dbg(LOG_NOTICE, "%s, got %d %d %d\n", NAME, n, idx1, idx2);
		if(data[idx2] == ',' || idx2 == n) {
			data[idx2] = '\0';
			ret++;
			int val = atoi(data + idx1);
			if(cpu_idx == val) {
				close(fd);
				return ret;
			}
			idx2++;
			idx1 = idx2;
		} else {
			idx2++;
		}
	}
	close(fd);
	return -1;
}


char get_index(int cpu_idx, char *path) {
	char filename[128];
	int fd;
	char data[8];
	sprintf(filename, path, cpu_idx);
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	if (pread(fd, (void *)&data, sizeof data, 0) < 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return (char)atoi((const char*)data);
}




void get_fms() {
	unsigned int eax, ebx, ecx, edx;
	unsigned int fms;
	__cpuid(1, fms, ebx, ecx, edx);
	family = (fms >> 8) & 0xf;
	model = (fms >> 4) & 0xf;
	//stepping = fms & 0xf;
	if (family == 0xf)
		family += (fms >> 20) & 0xff;
	if (family >= 6)
		model += ((fms >> 16) & 0xf) << 4;
}



int init_cpu_topo() {
	int i = 0;
	memset((void *)topo, 0xff, sizeof(short) * MAX_PKG * MAX_CORE * MAX_HT);
	DIR* dir;
	dir = opendir(CPU_LIST);
	if(dir == NULL) {
		return -1;
	}
	struct dirent * ent;
	while((ent = readdir(dir)) != NULL) {
		if(!isdigit(ent->d_name[0])) {
			continue;
		}
		int idx = atoi(ent->d_name);
		char pidx = get_index(idx, PKG_ID_FILE);
		char cidx = get_index(idx, CORE_ID_FILE);
		char hidx = get_ht_index(idx);

		if(pidx >= 0 && cidx >= 0 && hidx >= 0) {
			topo[pidx][cidx][hidx] = (short)idx;
			nr_lcpu++;
			if(pidx > nr_pkg) {
				nr_pkg = pidx;
			}
			if(cidx > core_id_max) {
				core_id_max = cidx;
			}
			if(hidx > ht_per_core) {
				ht_per_core = pidx;
			}
		}
	}

	nr_pkg++;
	ht_per_core++;

	for(i = 0; i < core_id_max + 1; i++) {
		if(topo[0][i][0] >=0) {
			core_per_pkg++;
		}
	}

	closedir(dir);
}

int board_info() {
	bvname[0] = '\0';
	pname[0] = '\0';
	int len = 0;
	FILE *fp = fopen(BOARD_VENDOR_FILE, "r");
	if(fp) {
		fgets(bvname, 63, fp);
		len = strlen(bvname);
		if(len > 0) {
			bvname[len - 1] = '\0';
		}
		fclose(fp);
	}
	fp = fopen(PRODUCT_NAME_FILE, "r");
	if(fp) {
		fgets(pname, 63, fp);
		len = strlen(pname);
		if(len > 0) {
			pname[len - 1] = '\0';
		}
		fclose(fp);
	}
	return 0;
}

////////////////////////////////////////////

inline int begin_with(const char* a, const char* sub) {
	return (a == strstr(a, sub));
}


int get_topo(char* csrow_dir_name, int channel) {

	char channel_dimm_label_name[256];
	char channel_ce_count_name[256];

	snprintf(channel_dimm_label_name, 255, "%sch%d_dimm_label", csrow_dir_name, channel);
	//printf("%s\n", channel_dimm_label_name);
	snprintf(channel_ce_count_name, 255, "%sch%d_ce_count", csrow_dir_name, channel);
	//printf("%s\n", channel_ce_count_name);

	FILE *fp = fopen(channel_dimm_label_name, "r");
	if(fp == NULL) {
		return 1;
	}

	FILE *fp2 = fopen(channel_ce_count_name, "r");
	if(fp == NULL) {
		fclose(fp);
		return 1;
	}

	int a = -1, b = -1, c = -1, d = -1, val = -1;
	int n = EOF;
	int done = 0;

	// sandybridge & ivybridge
	// E5-2697 v2 @12 cores with two MC
	n = fscanf(fp, "CPU_SrcID#%d_Channel#%d_DIMM#%d", &a, &c, &d);
	if(n == 3) {
		done = 1;
		b = 0;
	}

	if(!done) {
		a = -1; b = -1; c = -1; d = -1;
		rewind(fp);
		// haswell & broadwell
		n = fscanf(fp, "CPU_SrcID#%d_Ha#%d_Chan#%d_DIMM#%d", &a, &b, &c, &d);
		if(n == 4) {
			done = 1;
		}
	}

	if(!done) {
		a = -1; b = -1; c = -1; d = -1;
		rewind(fp);
		// skylake
		n = fscanf(fp, "CPU_SrcID#%d_MC#%d_Chan#%d_DIMM#%d", &a, &b, &c, &d);
		if(n == 4) {
			done = 1;
		}
	}

	fscanf(fp2, "%d", &val);

	fclose(fp);
	fclose(fp2);

	int i;
	int topo = a * 4096 + b * 256 + c * 16 + d;
	for(i = 0; i < MAX_DIMM_SLOT; i++) {
			//printf("%04X %04X %d %d\n", dimm_info_data[i].topo & 0xFFFF, topo, dimm_info_data[i].seq, dimm_info_data[i].sz_gb);
			if(dimm_info_data[i].seq != 0 && dimm_info_data[i].sz_gb > 0  &&
					((dimm_info_data[i].topo & 0x0000FFFF) == topo)) {
				int p1 = -1;
				int p2 = -1;
				int p3 = -1;
				int nret = 0;
				nret = sscanf(channel_dimm_label_name, "/sys/devices/system/edac/mc/mc%d/csrow%d/ch%d_dimm_label",
						&p1, &p2, &p3);
				if(nret == 3) {
					dimm_info_data[i].edac_path = p1 * 256 + p2 * 16 + p3;
				} else {
					dimm_info_data[i].edac_path = -1;
				}
				//printf("%2d %s %04X\t%s\t%d GiB\t%d-%d-%d-%d\t%d\n", i + 1, channel_dimm_label_name,
				//		dimm_info_data[i].edac_path, dimm_info_data[i].dloc, dimm_info_data[i].sz_gb, a, b, c, d, val);
				break;
			}
	}
	// done maybe 0 here
	//printf("%s\t%d-%d-%d-%d\t%d\n", channel_dimm_label_name, a, b, c, d, val);

}



// https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_2.7.1.pdf
// https://www.dmtf.org/sites/default/files/standards/documents/DSP0134_3.1.1.pdf
// 7.18 Memory Device (Type 17)

/*
	root@in16-014^:~#
	#hexdump -C /sys/firmware/dmi/entries/17-0/raw
	----------00-01-02-03-04-05-06-07--08-09-0A-0B-0C-0D-0E-0F-------------------
	00000000  11 22 1e 00 1d 00 2e 00  40 00 40 00 00 20 09 00  |."......@.@.. ..|
	00000010  01 02 18 80 20 35 05 03  04 05 06 02 00 00 00 00  |.... 5..........|
	00000020  36 05 44 49 4d 4d 5f 41  31 00 43 50 55 31 00 32  |6.DIMM_A1.CPU1.2|
	00000030  43 30 30 42 33 30 30 32  43 30 30 00 33 42 45 43  |C00B3002C00.3BEC|
	00000040  37 37 39 33 00 30 46 31  35 31 33 36 33 00 33 36  |7793.0F151363.36|
	00000050  4b 53 46 31 47 37 32 50  5a 2d 31 47 34 4d 31 00  |KSF1G72PZ-1G4M1.|
	00000060  00                                                |.|
	00000061

 *
 * # hexdump -C /sys/firmware/dmi/entries/17-0/raw
 * 	----------00-01-02-03-04-05-06-07--08-09-0A-0B-0C-0D-0E-0F-------------------
	00000000  11 28 00 11 00 10 fe ff  48 00 40 00 00 40 09 01  |.(......H.@..@..|
	00000010  01 00 1a 80 20 60 09 02  03 04 05 02 00 00 00 00  |.... `..........|
	00000020  55 08 b0 04 b0 04 b0 04  41 31 00 30 30 32 43 30  |U.......A1.002C0|
	00000030  36 33 32 30 30 32 43 00  31 32 35 30 37 44 34 37  |632002C.12507D47|
	00000040  00 30 30 31 36 31 35 43  30 00 31 38 41 53 46 32  |.001615C0.18ASF2|
	00000050  47 37 32 50 44 5a 2d 32  47 33 42 31 00 00        |G72PDZ-2G3B1..|

 *  10.26.18.209
	# hexdump -C /sys/firmware/dmi/entries/17-0/raw
	----------00-01-02-03-04-05-06-07--08-09-0A-0B-0C-0D-0E-0F-------------------
	00000000  11 28 1e 00 1d 00 fe ff  48 00 40 00 00 40 09 00  |.(......H.@..@..|
	00000010  01 02 1a 80 00 6a 0a 03  04 05 06 01 00 00 00 00  |.....j..........|
	00000020  60 09 b0 04 b0 04 b0 04  50 31 2d 44 49 4d 4d 41  |`.......P1-DIMMA|
	00000030  31 00 50 30 5f 4e 6f 64  65 30 5f 43 68 61 6e 6e  |1.P0_Node0_Chann|
	00000040  65 6c 30 5f 44 69 6d 6d  30 00 53 61 6d 73 75 6e  |el0_Dimm0.Samsun|
	00000050  67 00 34 30 30 38 43 42  30 30 00 50 31 2d 44 49  |g.4008CB00.P1-DI|
	00000060  4d 4d 41 31 5f 41 73 73  65 74 54 61 67 20 28 64  |MMA1_AssetTag (d|
	00000070  61 74 65 3a 31 38 2f 32  39 29 00 4d 33 39 33 41  |ate:18/29).M393A|
	00000080  32 4b 34 30 42 42 32 2d  43 54 44 20 20 20 20 00  |2K40BB2-CTD    .|
	00000090  00                                                |.|
	00000091

 *  root@n20-158-146:~# hexdump -C /sys/firmware/dmi/entries/17-0/raw
  	----------00-01-02-03-04-05-06-07--08-09-0A-0B-0C-0D-0E-0F-------------------
	00000000  11 28 24 00 23 00 ff ff  48 00 40 00 ff 7f 09 00  |.($.#...H.@.....|
	00000010  01 02 1a 80 80 6a 0a 03  04 05 06 04 00 00 01 00  |.....j..........|
	00000020  60 09 b0 04 b0 04 b0 04  44 49 4d 4d 30 30 30 00  |`.......DIMM000.|
	00000030  5f 4e 6f 64 65 30 5f 43  68 61 6e 6e 65 6c 30 5f  |_Node0_Channel0_|
	00000040  44 69 6d 6d 30 00 48 79  6e 69 78 00 33 33 31 33  |Dimm0.Hynix.3313|
	00000050  35 35 43 31 00 31 38 32  37 20 00 48 4d 41 41 38  |55C1.1827 .HMAA8|
	00000060  47 4c 37 41 4d 52 34 4e  2d 56 4b 20 20 20 20 00  |GL7AMR4N-VK    .|
	00000070  00                                                |.|
	00000071

 */

char *get_dmi_entry_str(char *begin, char *end, int seq) {
	int i = 1;

	while(begin < end) {
		if(i >= seq) {
			break;
		}
		if(*begin == '\0') {
			i++;
		}
		begin++;
	}

	if(begin < end && i == seq) {
		return begin;
	} else {
		return NULL;
	}
}

int decode_dmi_dimm() {
	// /sys/firmware/dmi/entries/17-
	// DMI_ENTRY_DIR

	memset(dimm_info_data, 0, MAX_DIMM_SLOT * sizeof(struct dimm_info));

	DIR* dir;
	dir = opendir(DMI_ENTRY_DIR);
	if(dir == NULL) {
		// error
		return 1;
	}

	struct dirent * ent;
	while((ent = readdir(dir)) != NULL) {
		if(strncmp(ent->d_name, DMI_TYPE_DIMM_STR, strlen(DMI_TYPE_DIMM_STR)) != 0) {
			// skip those non MC*
			continue;
		}
		char dmi_dimm_entry_name[256];
		char buf[4096];
		snprintf(dmi_dimm_entry_name, 255, "%s%s/raw", DMI_ENTRY_DIR, ent->d_name);

		int seq = -2;
		sscanf(ent->d_name, "17-%d", &seq);
		seq++;

		FILE *fp = fopen(dmi_dimm_entry_name, "r");
		if(fp == NULL) {
			continue;
		}
		int n = fread(buf, 1, 1024, fp);
		fclose(fp);

		int n_entry_len = (int) buf[1];

		int size_mibyte = *(short *) (buf + 0x0C);
		// size >= 32GB
		if(size_mibyte == 0x7fff) {
			size_mibyte = *(int *) (buf + 0x1C);
		}

		int dloc_seq = buf[0x10];
		int bloc_seq = buf[0x11];
		int sn_seq = buf[0x18];
		int pn_seq = buf[0x1A];
		char *begin = buf + n_entry_len;
		char *end = buf + n_entry_len + n - 1;
		//34 bytes, 40 bytes, difference is the voltage information
		//printf("%s:\t%3d %3d / %2d / %d %d %d %d / %d / %s / %s / %s / %s\n", dmi_dimm_entry_name, n, n_entry_len,
		//		seq, dloc_seq, bloc_seq, sn_seq, pn_seq, size_mibyte,
		//		(dloc_seq == 0 ? "NONE" : get_dmi_entry_str(begin, end, dloc_seq)),
		//		(bloc_seq == 0 ? "NONE" : get_dmi_entry_str(begin, end, bloc_seq)),
		//		(pn_seq == 0 ? "NONE" : get_dmi_entry_str(begin, end, pn_seq)),
		//		(sn_seq == 0 ? "NONE" : get_dmi_entry_str(begin, end, sn_seq)) );

		if(seq > MAX_DIMM_SLOT) {
			// WARN TODO
			continue;
		}

		// buggy bios may have flash chip in dmidecode, fuck XXX
		if(size_mibyte > 0 && size_mibyte < 1024) {
			// WARN TODO
			continue;
		}

		dimm_info_data[seq - 1].seq = seq;
		dimm_info_data[seq - 1].topo = -1;
		dimm_info_data[seq - 1].sz_gb = size_mibyte / 1024;

		if(size_mibyte >= 1024) {
			dimm_plugged++;
		}
		nr_dimm_slots++;

		char *tmp_ptr = NULL;
		if((dloc_seq > 0) && (tmp_ptr = get_dmi_entry_str(begin, end, dloc_seq)) != NULL) {
			sprintf(dimm_info_data[seq - 1].dloc, "%s", tmp_ptr);
		} else {
			dimm_info_data[seq - 1].dloc[0] = '\0';
		}
		if((bloc_seq > 0) && (tmp_ptr = get_dmi_entry_str(begin, end, bloc_seq)) != NULL) {
			sprintf(dimm_info_data[seq - 1].bloc, "%s", tmp_ptr);
		} else {
			dimm_info_data[seq - 1].bloc[0] = '\0';
		}
		if((pn_seq > 0) && (tmp_ptr = get_dmi_entry_str(begin, end, pn_seq)) != NULL) {
			sprintf(dimm_info_data[seq - 1].pn, "%s", tmp_ptr);
		} else {
			dimm_info_data[seq - 1].pn[0] = '\0';
		}
		if((sn_seq > 0) && (tmp_ptr = get_dmi_entry_str(begin, end, sn_seq)) != NULL) {
			sprintf(dimm_info_data[seq - 1].sn, "%s", tmp_ptr);
		} else {
			dimm_info_data[seq - 1].sn[0] = '\0';
		}

		dimm_info_data[seq - 1].topo = -1;
		dimm_info_data[seq - 1].edac_path = -1;
		dimm_info_data[seq - 1].edac_val = -2;

		//printf("%s:\t%3d %3d / %2d / %d %d %d %d / %d / %s / %s / %s / %s\n", dmi_dimm_entry_name, n, n_entry_len,
		//		seq, dloc_seq, bloc_seq, sn_seq, pn_seq, size_mibyte,
		//		dimm_info_data[seq - 1].dloc, dimm_info_data[seq - 1].bloc,
		//		dimm_info_data[seq - 1].pn, dimm_info_data[seq - 1].sn );

	}
	closedir(dir);
}

// EP only, EN and EX not covered, more details need the "displayed name"
int channels_per_agent(int family, int model, int ncore) {
	// so far, Intel only, family ignore (06)
	switch(model) {
		// E5-2690, 8 Cores, 2.9/3.8GHz/20MB/DDR3 1600, 32nm
		case (0x2D):
			return 4;

		// E5-2697 v2, 12 Cores, 2.7/3.5GHz/30MB/DDR3 1866, 20nm
		case (0x3E):
			if(ncore > 10) {
				return 2;
			}
			return 4;

		// v3, E5-2650V3 at 10 cores
		case (0x3F):
			if(ncore > 8) {
				return 2;
			}
			return 4;

		// v4, E5-2650 v4 at 12 cores
		case (0x4F):
			if(ncore > 10) {
				return 2;
			}
			return 4;

		// skylake
		case (0x55):
			return 3;

		default:
			return 4;
	}
}

// last case is DELL
// 10.6.130.14 C6220, 16 slots
// 10.3.21.218 C6320, 16 slots
// 10.8.163.166 PowerEdge R730xd, 24 slots
// 10.63.6.31 (CDN) R640 Purley, 24 slots, A1-A12, B1-B12
// 10.26.26.77 C6420 Purley, 16 slots, A1-A8, B1-B8
// 10.23.215.16 R540 Purley, 16 slots, A1-A10, B1-B6

int parse_dimm_path_dell(char *loc1, char* loc2, char* server_vendor, char* server_model, int cpu_family, int cpu_model) {

	int cpu = -1;
	int slot = -1;
	int ha = -1;
	int ch = -1;
	int dimm = -1;

	int dell_slot_idx = -1;
	int dell_need_fix_unbalance = 0;
	// snb, ivb, hsw, bdw
	int dell_slots_per_socket = 4;

	int ch_per_agent = channels_per_agent(cpu_family, cpu_model, core_per_pkg);

	// skylake
	if(cpu_model == 0x55) {
		dell_slots_per_socket = 6;
		if(nr_dimm_slots == 16) {
			dell_need_fix_unbalance = 1;
		}
	}

	if(begin_with(loc1, "DIMM_A") || begin_with(loc1, "A")) {
		cpu = 0;
	}

	if(begin_with(loc1, "DIMM_B") || begin_with(loc1, "B")) {
		cpu = 1;
	}

	// parse failed
	if(cpu < 0) {
		return -1;
	}

	if(begin_with(loc1, "DIMM_")) {
		sscanf(loc1 + 6, "%d", &dell_slot_idx);
	} else {
		sscanf(loc1 + 1, "%d", &dell_slot_idx);
	}

	dell_slot_idx--;

	dimm = dell_slot_idx / dell_slots_per_socket;
	ha = (dell_slot_idx % dell_slots_per_socket) / ch_per_agent;
	ch = (dell_slot_idx % dell_slots_per_socket) % ch_per_agent;
	slot = ha * ch_per_agent + ch;

	// fix the case such as C6420 which uses 16 slots only out of 24
	// C6420: 8 + 8 slots
	// R540: 10 + 6 slots
	if(dell_need_fix_unbalance == 1) {
		if(strstr(server_model, "R540") != NULL) {
			if(dell_slot_idx == 8) {
				ha = 1;
				ch = 0;
			}
			if(dell_slot_idx == 9) {
				ha = 1;
				ch = 1;
			}
		}

		if(strstr(server_model, "C6420") != NULL) {
			if(dell_slot_idx == 7) {
				ha = 1;
				ch = 0;
			}
		}

		slot = ha * ch_per_agent + ch;
	}

	//printf("*DEBUG* %d-%d-%d-%d / %d-%d-%d\n", cpu, ha, ch, dimm, cpu, slot ,dimm);
	return ch_per_agent * 65536 + cpu * 4096 + ha * 256 + ch * 16 + dimm;

}

// 2 socket server only
int parse_dimm_path(char *loc1, char* loc2, char* server_vendor, char* server_model,
		int cpu_family, int cpu_model) {
	int done = 0;

	//char cap = '\0';
	int a = -1;
	int b = -1;
	int c = -1;
	int d = -1;

	int cpu = -1;
	int slot = -1;
	int ha = -1;
	int ch = -1;
	int dimm = -1;

	int ch_per_agent = channels_per_agent(cpu_family, cpu_model, core_per_pkg);

	// Huawei Server, 2U4, 16 slots
	// DIMM131 | BRANCH 1 CHANNEL 3 DIMM 1 (10.11.65.14, Tecal XH321 V2, E5-2630v2)
	// DIMM150 | _Node1_Channel5_Dimm0 (10.25.47.67, XH321 V5, Silver4114)

	if(!done && begin_with(loc1, "DIMM") && begin_with(loc2, "BRANCH ")) {
		int n = sscanf(loc2, "BRANCH %d CHANNEL %d DIMM %d", &a, &b, &c);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c);
		if(n == 3) {
			done = 1;
			cpu = a;
			slot = b;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = c;
		}
	}

	if(!done && begin_with(loc1, "DIMM") && begin_with(loc2, "_Node")) {
		int n = sscanf(loc2, "_Node%d_Channel%d_Dimm%d", &a, &b, &c);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n == 3) {
			done = 1;
			cpu = a;
			slot = b;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = c;
		}
	}

	// SuperMicro (Sugon, Inspur, Powerleader ...) 2U4, 16 slots, Grantly
	// Powerleader  10.12.56.161
	// P1-DIMMD2 | P0_Node0_Channel3_Dimm1
	// P2-DIMMH2 | P1_Node1_Channel3_Dimm1

	if(!done && loc1[0] == 'P' && loc2[0] == 'P'
			&& (ch_per_agent == 2 || ch_per_agent == 4)) {
		int n = sscanf(loc2, "P%d_Node%d_Channel%d_Dimm%d", &a, &b, &c, &d);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n == 4) {
			done = 1;
			cpu = a;
			slot = c;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = d;
		}
	}

	// SuperMicro (Sugon, Powerleader ...) 2U4, 24 slots, Purley
	// Sugon  10.27.57.13
	// P1-DIMMF1 | P0_Node1_Channel2_Dimm0
	// P2-DIMMF1 | P1_Node1_Channel2_Dimm0

	if(!done && loc1[0] == 'P' && loc2[0] == 'P' && ch_per_agent == 3) {
		int n = sscanf(loc2, "P%d_Node%d_Channel%d_Dimm%d", &a, &b, &c, &d);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n == 4) {
			done = 1;
			cpu = a;
			ha = b;
			ch = c;
			slot = ha * ch_per_agent + ch;
			dimm = d;
		}
	}


	// Inspur 2U4 Purley (i24) 16 slots,  Inspur 5212M5 Purley 24 slots
	// CPU0_C5D0 | NODE 2 (10.25.78.103)
	// CPU0_CH4_DIMM1 | NODE 2 (10.24.30.39)

	if(!done && begin_with(loc2, "NODE")) {
		int n = sscanf(loc1, "CPU%d_CH%d_DIMM%d", &a, &b, &c);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n != 3) {
			n = sscanf(loc1, "CPU%d_C%dD%d", &a, &b, &c);
			//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		}
		if(n == 3) {
			done = 1;
			cpu = a;
			slot = b;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = c;
		}
	}

	// Inspur 5212M4 (10.3.14.14)
	// CH{A-H}0 | A1_Node?_Channel?_Dimm?

	if(!done && begin_with(loc1, "CH") && loc2[0] == 'A') {
		int n = sscanf(loc2, "A1_Node%d_Channel%d_Dimm%d", &a, &b, &c);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n == 3) {
			done = 1;
			cpu = a;
			slot = b;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = c;
		}
	}

	// ??? Sugon I620-G30, 24 slots (10.23.219.74)
	// Only Grantly or above: CPU0_DIMMD1    | D1_Node0_Channel3_Dimm1
	// Purley: CPU0_DIMM_E1   | A0_Node1_Channel4_Dimm1

	if(!done && (begin_with(loc1, "CPU0_DIMM_") || begin_with(loc1, "CPU1_DIMM_"))
			&& loc2[0] == 'A') {
		int n = sscanf(loc2, "A%d_Node%d_Channel%d_Dimm%d", &a, &b, &c, &d);
		//printf("*DEBUG* %d %d %d %d\n", a, b, c, d);
		if(n == 4) {
			done = 1;
			cpu = a;
			slot = c;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = d;
		}
	}


	// ??? Sugon I620-G20S, 24 slots (10.20.152.130)
	// Only Grantly or above: CPU0_DIMMD1    | D1_Node0_Channel3_Dimm1
	// Purley: CPU0_DIMM_E1   | A0_Node1_Channel4_Dimm1

	if(!done && (begin_with(loc1, "CPU0_DIMM") || begin_with(loc1, "CPU1_DIMM"))) {
		int n = sscanf(loc2 + 1, "%d_Node%d_Channel%d_Dimm%d", &a, &b, &c, &d);
		//printf("*DEBUG* %c %d %d %d %d\n", loc2[0], a, b, c, d);
		if(n == 4) {
			done = 1;
			cpu = b;
			slot = c;
			ha = slot / ch_per_agent;
			ch = slot % ch_per_agent;
			dimm = d;
		}
	}

	// DELL
	if(!done && (strstr(server_vendor, "Dell") != NULL)) {
		return parse_dimm_path_dell(loc1, loc2, server_vendor, server_model, cpu_family, cpu_model);
	}

	if(!done) {
		return -1;
	}

	// printf("*DEBUG* %d-%d-%d-%d / %d-%d-%d\n", cpu, ha, ch, dimm, cpu, slot ,dimm);
	return ch_per_agent * 65536 + cpu * 4096 + ha * 256 + ch * 16 + dimm;

}

// should called when edac driver load or unload, may scheduled periodicaly
int edac_sysfs_decode() {
	DIR* dir;
	dir = opendir(EDAC_MC_DIR);
	if(dir == NULL) {
		// error
		return 1;
	}

	struct dirent * ent;
	while((ent = readdir(dir)) != NULL) {
		if(strncmp(ent->d_name, MC_STR, strlen(MC_STR)) != 0) {
			// skip those non MC*
			continue;
		}
		char mc_dir_name[256];
		snprintf(mc_dir_name, 255, "%s%s/", EDAC_MC_DIR, ent->d_name);
		DIR* dir_mc;
		dir_mc = opendir(mc_dir_name);
		if(dir_mc == NULL) {
			// error
			continue;
		}

		struct dirent * ent_csrow;
		while((ent_csrow = readdir(dir_mc)) != NULL) {
			if(strncmp(ent_csrow->d_name, CSROW_STR, strlen(CSROW_STR)) != 0) {
				// skip those non csrow*
				continue;
			}
			char csrow_dir_name[256];
			snprintf(csrow_dir_name, 255, "%s%s/", mc_dir_name, ent_csrow->d_name);
			DIR* dir_csrow;
			dir_csrow = opendir(csrow_dir_name);
			if(dir_csrow == NULL) {
				// error
				continue;
			}

			struct dirent * ent_channel;
			while((ent_channel = readdir(dir_csrow)) != NULL) {
				if(strncmp(ent_channel->d_name, CHANNEL_STR, strlen(CHANNEL_STR)) != 0) {
					// skip those non ch*
					continue;
				}

				// assume channel idx 0-9, FIXME later for over 10 channels, TODO
				if(ent_channel->d_name[4] == 'c') {
					int channel = -1;
					int matched = sscanf(ent_channel->d_name, "ch%d_ce_count", &channel);
					if(matched != 1) {
						continue;
					}
					get_topo(csrow_dir_name, channel);
				}
			}
			closedir(dir_csrow);
		}
		closedir(dir_mc);
	}
	closedir(dir);

	return 0;
}

void get_edac_val() {
	char path[256];
	int i = 0;
	for(i = 0; i < nr_dimm_slots; i++) {
		//assert(dimm_info_data[i].seq != 0)
		if(dimm_info_data[i].edac_path < 0) {
			;
			continue;
		}
		int val = -1;

		int p1 = (dimm_info_data[i].edac_path & 0x00000F00) / 256;
		int p2 = (dimm_info_data[i].edac_path & 0x000000F0) / 16;
		int p3 = (dimm_info_data[i].edac_path & 0x0000000F);
		sprintf(path, "/sys/devices/system/edac/mc/mc%d/csrow%d/ch%d_ce_count", p1, p2, p3);

		FILE *fp = fopen(path, "r");
		if(fp == NULL) {
			fclose(fp);
			continue;
		}

		fscanf(fp, "%d", &val);
		fclose(fp);

		dimm_info_data[i].edac_val = val;

		printf("%2d - %2d GiB\t/ %08X / %s / %s / %s / %s / %s / %d\n", dimm_info_data[i].seq,
			dimm_info_data[i].sz_gb, dimm_info_data[i].topo, dimm_info_data[i].dloc,
			dimm_info_data[i].bloc, dimm_info_data[i].pn, dimm_info_data[i].sn, path,
			dimm_info_data[i].edac_val);
	}
}

int main(int argc, char* argv[]) {
	get_fms();
	init_cpu_topo();
	board_info();

	decode_dmi_dimm();
	printf("### %s / %s / %02X_%02X / %d / %d / %d\n", bvname, pname, family, model,
			core_per_pkg, channels_per_agent(family, model, core_per_pkg), dimm_plugged);

	int i = 0;
	for(i = 0; i < MAX_DIMM_SLOT; i++) {
		if(dimm_info_data[i].seq != 0) {
			int loc = -1;
			if(dimm_info_data[i].sz_gb > 0) {
				loc = parse_dimm_path(dimm_info_data[i].dloc, dimm_info_data[i].bloc,
						bvname, pname, family, model);
			}
			dimm_info_data[i].topo = loc;
			//printf("%2d - %2d GiB\t/ %08X / %s / %s / %s / %s\n", dimm_info_data[i].seq,
			//		dimm_info_data[i].sz_gb, dimm_info_data[i].topo,
			//		dimm_info_data[i].dloc, dimm_info_data[i].bloc,
			//		dimm_info_data[i].pn, dimm_info_data[i].sn );
		}
	}

	edac_sysfs_decode();



	for(i = 0; i < nr_dimm_slots; i++) {
		//assert(dimm_info_data[i].seq != 0)
		printf("%2d - %2d GiB\t/ %08X / %s / %s / %s / %s / %04hX\n", dimm_info_data[i].seq,
			dimm_info_data[i].sz_gb, dimm_info_data[i].topo, dimm_info_data[i].dloc,
			dimm_info_data[i].bloc, dimm_info_data[i].pn, dimm_info_data[i].sn,
			(unsigned short)dimm_info_data[i].edac_path);
	}

	printf("\n");

	get_edac_val();

	return 0;
}

// 062D, 063E (n6-128-014, 10.11.64.14), 063F, 064F, 0655
