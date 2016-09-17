#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>

#include "main.h" //for handle_file
#include "mfile.h"
#include "hisense.h"
#include "lzhs/lzhs.h"
#include "util.h"

MFILE *is_hisense(const char *pkgfile){
	MFILE *mf = mopen(pkgfile, O_RDONLY);
	if(!mf){
		err_exit("Cannot open file %s\n", pkgfile);
	}
	
	uint8_t *data = mdata(mf, uint8_t);
	data = &data[UPG_HEADER_SIZE];

	/* First pak doesn't have OTA ID fields */
	struct hipkg *cfig = (struct hipkg *)data;
	if(
		(!strcmp(cfig->pakName, "cfig")) &&
		(!strncmp(cfig->data, "START", 5))
	){
		/* Checking for END may be desirable */
		return mf;
	}
	mclose(mf);
	return NULL;
}

MFILE *is_lzhs_fs(const char *pkg){
	MFILE *mf = mopen(pkg, O_RDONLY);
	if(!mf){
		err_exit("Cannot open file %s\n", pkg);
	}

	uint8_t *data = mdata(mf, uint8_t);

	if(
		msize(mf) > (HISENSE_EXT_LZHS_OFFSET + sizeof(struct lzhs_header)) &&
		is_lzhs_mem(mf, HISENSE_EXT_LZHS_OFFSET) &&
		is_lzhs_mem(mf, HISENSE_EXT_LZHS_OFFSET + sizeof(struct lzhs_header)) &&
		// first LZHS header contains number of block in checksum
		((struct lzhs_header *)&data[HISENSE_EXT_LZHS_OFFSET])->checksum != 0x00
	){
		return mf;
	}

	mclose(mf);
	return NULL;
}

/*
 * Hisense (or Mediatek?) uses an ext4 filesystem splitted in chunks, compressed with LZHS
 * They use 2 LZHS header for each chunk
 * The first header contains the chunk number, and the compressed size includes the outer lzhs header (+16)
 * The second header contains the actual data
 */
void extract_lzhs_fs(MFILE *mf, const char *dest_file){
	uint8_t *data = mdata(mf, uint8_t) + HISENSE_EXT_LZHS_OFFSET;
	
	FILE *out_file = fopen(dest_file, "w+");
	if(!out_file){
		err_exit("Cannot open %s for writing\n", dest_file);
	}

	#ifdef LZHSFS_EXTRACT_CHUNKS
	char *dir = my_dirname(dest_file);
	char *file = my_basename(dest_file);
	char *base = remove_ext(file);
	#endif


	printf("Copying 0x%08X bytes\n", HISENSE_EXT_LZHS_OFFSET);
	/* Copy first MB as-is (uncompressed) */
	fwrite (
		mdata(mf, uint8_t),
		HISENSE_EXT_LZHS_OFFSET,
		1,
		out_file
	);

	while(moff(mf, data) < msize(mf)){
		struct lzhs_header *main_hdr = (struct lzhs_header *)data; 
		struct lzhs_header *seg_hdr = (struct lzhs_header *)(data + sizeof(*main_hdr));

		printf("\n[0x%08X] segment #%u (compressed='%u bytes', uncompressed='%u bytes')\n",
			moff(mf, main_hdr),
			main_hdr->checksum,
			seg_hdr->compressedSize, seg_hdr->uncompressedSize);

		uint8_t out_checksum = 0x00;

		#ifdef LZHSFS_EXTRACT_CHUNKS
		char *out;

		asprintf(&out, "%s/%s.%d", dir, base, main_hdr->checksum);
		lzhs_decode(mf, moff(mf, seg_hdr), out, &out_checksum);
		free(out);
		#else
		cursor_t *out_cur = lzhs_decode(mf, moff(mf, seg_hdr), NULL, &out_checksum);
		if(out_cur == NULL || (intptr_t)out_cur < 0){
			err_exit("LZHS decode failed\n");
		}

		fwrite(out_cur->ptr, out_cur->size, 1, out_file);
		free(out_cur);
		#endif

		uint pad;
		pad = (pad = (seg_hdr->compressedSize % 16)) == 0 ? 0 : (16 - pad);

		data += (
			sizeof(*main_hdr) + sizeof(*seg_hdr) +
			seg_hdr->compressedSize +
			pad
		);
	}

	fclose(out_file);

	#ifdef LZHSFS_EXTRACT_CHUNKS
	free(dir); free(file); free(base);
	#endif
}

void extract_hisense(MFILE *mf, struct config_opts_t *config_opts){
	uint8_t *data = mdata(mf, uint8_t) + UPG_HEADER_SIZE;

	char *file_name = my_basename(mf->path);
	char *file_base = remove_ext(file_name);

	off_t i = UPG_HEADER_SIZE;
	
	int pakNo;
	for(pakNo=0; i < msize(mf); pakNo++){
		struct hipkg *pak = (struct hipkg *)data;
		/* End of package */
		if(pak->size == 0){
			break;
		}

		printf("PAK #%u (name='%s', offset='0x%lx', size='%u bytes'",
			pakNo + 1, pak->pakName, moff(mf, data), pak->size
		);

		data += sizeof(*pak);

		uint8_t *pkgData = pak->data;
		size_t pkgSize = pak->size;

		char *dest_path;
		struct hipkg_plat *ext = (struct hipkg_plat *)pkgData;
		if(!strncmp(ext->platform, HISENSE_MTK_MAGIC, strlen(HISENSE_MTK_MAGIC))){
			printf(", platform='%s', otaid='%s')\n", ext->platform, ext->otaID);
			if(pakNo == 1){
				sprintf(config_opts->dest_dir, "%s/%s", config_opts->dest_dir, ext->otaID);
				createFolder(config_opts->dest_dir);
			}
			pkgData += sizeof(*ext) + ext->otaID_len;
			pkgSize -= sizeof(*ext) + ext->otaID_len;
		} else {
			printf(")\n");
		}

		struct hipkg_pad *pad = (struct hipkg_pad *)pkgData;
		if(!strncmp(pad->magic, HISENSE_PAD_MAGIC, strlen(HISENSE_PAD_MAGIC))){
			pkgData += sizeof(*pad);
			pkgSize -= sizeof(*pad);
		}

		asprintf(&dest_path, "%s/%s.pak", config_opts->dest_dir, pak->pakName);

		MFILE *out = mfopen(dest_path, "w+");
		if(!out){
			err_exit("Cannot open %s for writing\n", dest_path);
		}

		printf("Saving partition (%s) to file %s\n\n", pak->pakName, dest_path);

		mfile_map(out, pkgSize);
		memcpy(
			mdata(out, void),
			pkgData,
			pkgSize
		);
		mclose(out);
		handle_file(dest_path, config_opts);
		free(dest_path);

		data += pak->size;
	}

	mclose(mf);
	mf = NULL;

	free(file_name);
	free(file_base);
}