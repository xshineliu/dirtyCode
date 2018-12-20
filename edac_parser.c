
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define TOPDIR "/sys/devices/system/edac/mc/"
#define MC_STR "mc"
#define CSROW_STR "csrow"
#define CHANNEL_STR "ch"


int get_topo(char* csrow_dir_name, int channel) {
	char channel_ce_count_name[256];
	char channel_dimm_label_name[256];

	snprintf(channel_ce_count_name, 255, "%sch%d_ce_count", csrow_dir_name, channel);
	//printf("%s\n", channel_ce_count_name);
	snprintf(channel_dimm_label_name, 255, "%sch%d_dimm_label", csrow_dir_name, channel);
	//printf("%s\n", channel_dimm_label_name);

	FILE *fp = fopen(channel_dimm_label_name, "r");
	if(fp == NULL) {
		return 1;
	}

	FILE *fp2 = fopen(channel_dimm_label_name, "r");
	if(fp == NULL) {
		fclose(fp);
		return 1;
	}

	int a = -1, b = -1, c = -1, d = -1;
	int n = EOF;
	int done = 0;
	// sandybridge & ivybridge
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

	// done maybe 0 here
	printf("%s\t%d\t%d\t%d\t%d\n", channel_dimm_label_name, a, b, c, d);

	fclose(fp);
	fclose(fp2);
}




int core() {
	DIR* dir;
	dir = opendir(TOPDIR);
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
		snprintf(mc_dir_name, 255, "%s%s/", TOPDIR, ent->d_name);
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

int main(int argc, char* argv[]) {
	core();
	return 0;
}
