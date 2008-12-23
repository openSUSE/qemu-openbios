/*
 *   Creation Date: <2002/10/02 22:24:24 samuel>
 *   Time-stamp: <2004/03/27 01:57:55 samuel>
 *
 *	<main.c>
 *
 *
 *
 *   Copyright (C) 2002, 2003, 2004 Samuel Rydh (samuel@ibrium.se)
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation
 *
 */


#include "openbios/config.h"
#include "openbios/bindings.h"
#include "openbios/elfload.h"
#include "openbios/nvram.h"
#include "libc/diskio.h"
#include "libc/vsprintf.h"
#include "kernel.h"
#include "ofmem.h"

//#define DEBUG_ELF

#ifdef DEBUG_ELF
#define ELF_DPRINTF(fmt, args...) \
do { printk("ELF - %s: " fmt, __func__ , ##args); } while (0)
#else
#define ELF_DPRINTF(fmt, args...) do { } while (0)
#endif

static void
transfer_control_to_elf( ulong elf_entry )
{
	ELF_DPRINTF("Starting ELF boot loader\n");
        call_elf( elf_entry );

	fatal_error("call_elf returned unexpectedly\n");
}

static int
load_elf_rom( ulong *elf_entry, int fd )
{
	int i, lszz_offs, elf_offs;
        char *addr;
	Elf_ehdr ehdr;
	Elf_phdr *phdr;
	size_t s;

	ELF_DPRINTF("Loading '%s' from '%s'\n", get_file_path(fd),
	       get_volume_name(fd) );

	/* the ELF-image (usually) starts at offset 0x4000 */
	if( (elf_offs=find_elf(fd)) < 0 ) {
                ELF_DPRINTF("----> %s is not an ELF image\n", get_file_path(fd));
		exit(1);
	}
	if( !(phdr=elf_readhdrs(fd, elf_offs, &ehdr)) )
		fatal_error("elf_readhdrs failed\n");

	*elf_entry = ehdr.e_entry;

	/* load segments. Compressed ROM-image assumed to be located immediately
	 * after the last segment */
	lszz_offs = elf_offs;
	for( i=0; i<ehdr.e_phnum; i++ ) {
		/* p_memsz, p_flags */
		s = MIN( phdr[i].p_filesz, phdr[i].p_memsz );
		seek_io( fd, elf_offs + phdr[i].p_offset );

		ELF_DPRINTF("filesz: %08lX memsz: %08lX p_offset: %08lX p_vaddr %08lX\n",
                   (ulong)phdr[i].p_filesz, (ulong)phdr[i].p_memsz, (ulong)phdr[i].p_offset,
                   (ulong)phdr[i].p_vaddr );

		if( phdr[i].p_vaddr != phdr[i].p_paddr )
			ELF_DPRINTF("WARNING: ELF segment virtual addr != physical addr\n");
		lszz_offs = MAX( lszz_offs, elf_offs + phdr[i].p_offset + phdr[i].p_filesz );
		if( !s )
			continue;
		if( ofmem_claim( phdr[i].p_vaddr, phdr[i].p_memsz, 0 ) == -1 )
			fatal_error("Claim failed!\n");

		addr = (char*)phdr[i].p_vaddr;
		if( read_io(fd, addr, s) != s )
			fatal_error("read failed\n");

		flush_icache_range( addr, addr+s );

		ELF_DPRINTF("ELF ROM-section loaded at %08lX (size %08lX)\n",
			    (ulong)phdr[i].p_vaddr, (ulong)phdr[i].p_memsz );
	}
	free( phdr );
	return lszz_offs;
}

static void
encode_bootpath( const char *spec, const char *args )
{
	phandle_t chosen_ph = find_dev("/chosen");
	set_property( chosen_ph, "bootpath", spec, strlen(spec)+1 );
	set_property( chosen_ph, "bootargs", args, strlen(args)+1 );
}

/************************************************************************/
/*	qemu booting							*/
/************************************************************************/
static void
try_path(const char *path, const char *param)
{
    ulong elf_entry;
    int fd;

    ELF_DPRINTF("Trying %s %s\n", path, param);
    if ((fd = open_io(path)) == -1) {
        ELF_DPRINTF("Can't open %s\n", path);
        return;
    }
    (void) load_elf_rom( &elf_entry, fd );
    close_io( fd );
    encode_bootpath( path, param );

    update_nvram();
    ELF_DPRINTF("Transfering control to %s %s\n",
                path, param);
    transfer_control_to_elf( elf_entry );
    /* won't come here */
}

/*
  Parse SGML structure like:
  <chrp-boot>
  <description>Debian/GNU Linux Installation on IBM CHRP hardware</description>
  <os-name>Debian/GNU Linux for PowerPC</os-name>
  <boot-script>boot &device;:\install\yaboot</boot-script>
  <icon size=64,64 color-space=3,3,2>

  CHRP system bindings are described at:
  http://playground.sun.com/1275/bindings/chrp/chrp1_7a.ps
*/
static void
try_bootinfo(const char *path)
{
    int fd;
    char tagbuf[256], bootscript[256], c, *left, *right;
    int len, tag, taglen, script, scriptlen;

    snprintf(bootscript, sizeof(bootscript), "%s,ppc\\bootinfo.txt", path);
    ELF_DPRINTF("Trying %s\n", bootscript);
    if ((fd = open_io(bootscript)) == -1) {
        ELF_DPRINTF("Can't open %s\n", bootscript);
        return;
    }
    len = read_io(fd, tagbuf, 11);
    tagbuf[11] = '\0';
    if (len < 0 || strcasecmp(tagbuf, "<chrp-boot>") != 0)
        goto badf;

    tag = 0;
    taglen = 0;
    script = 0;
    scriptlen = 0;
    do {
        len = read_io(fd, &c, 1);
        if (len < 0)
            goto badf;
        if (c == '<') {
            tag = 1;
            taglen = 0;
        } else if (c == '>') {
            tag = 0;
            tagbuf[taglen] = '\0';
            if (strcasecmp(tagbuf, "boot-script") == 0)
                script = 1;
            else if (strcasecmp(tagbuf, "/boot-script") == 0) {
                bootscript[scriptlen] = '\0';
                break;
            }
        } else if (tag && taglen < sizeof(tagbuf)) {
            tagbuf[taglen++] = c;
        } else if (script && scriptlen < sizeof(bootscript)) {
            bootscript[scriptlen++] = c;
        }
    } while (1);

    ELF_DPRINTF("got bootscript %s\n", bootscript);

    // Replace &device;: with original path
    push_str(bootscript);
    PUSH('&');
    fword("left-split");
    fword("2swap");
    PUSH(':');
    fword("left-split");
    fword("2drop");
    right = pop_fstr_copy();
    left = pop_fstr_copy();
    snprintf(bootscript, sizeof(bootscript), "%s%s,%s", left, path, right);
    ELF_DPRINTF("fixed bootscript %s\n", bootscript);

    feval(bootscript);
 badf:
    close_io( fd );
}

static void
yaboot_startup( void )
{
        static const char * const paths[] = { "hd:2,\\ofclient", "hd:2,\\yaboot" };
        static const char * const args[] = { "", "conf=hd:2,\\yaboot.conf" };
        char *path = pop_fstr_copy(), *param;
        int i;

        if (!path) {
            ELF_DPRINTF("Entering boot, no path\n");
            push_str("boot-device");
            push_str("/options");
            fword("(find-dev)");
            POP();
            fword("get-package-property");
            if (!POP()) {
                path = pop_fstr_copy();
                param = strchr(path, ' ');
                if (param) {
                    *param = '\0';
                    param++;
                } else {
                    push_str("boot-args");
                    push_str("/options");
                    fword("(find-dev)");
                    POP();
                    fword("get-package-property");
                    POP();
                    param = pop_fstr_copy();
                }
                try_bootinfo(path);
                try_path(path, param);
            }
        } else {
            ELF_DPRINTF("Entering boot, path %s\n", path);
            try_path(path, "");
            try_bootinfo(path);
        }
        for( i=0; i < sizeof(paths) / sizeof(paths[0]); i++ ) {
            try_path(paths[i], args[i]);
        }
	printk("*** Boot failure! No secondary bootloader specified ***\n");
}


/************************************************************************/
/*	entry								*/
/************************************************************************/

void
boot( void )
{
	fword("update-chosen");
	yaboot_startup();
}
