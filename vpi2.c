/*
	vpi2.c	(c) 1995-1999 Grant Guenther <grant@torque.net>
		(c) 1997-1999 David Campbell <campbell@torque.net>
		(c) 2000 Tim Waugh <twaugh@redhat.com>

	This is the ppSCSI protocol module for the Iomega VPI2 adapter
	found in the newer ZIP-100 drives.

*/

#error "This doesn't work yet."

#define	VPI2_VERSION	"0.91"

#define PPSC_BASE
#define PPSC_HA_MODULE

#include "ppscsi.h"

static char vpi2_map[256];	/* status bits permutation */

static void vpi2_init (PHA *pha)
{

/*  *** No MSG line on the VPI2 ! *** */ /* tmw: is this true for VPI2? */

/*                       { REQ, BSY, MSG,  CD,  IO}     */

	char key[5] = {0x80,0x40,0x00,0x20,0x10};

	ppsc_make_map(vpi2_map,key,0);
	sprintf(pha->ident,"vpi2 %s (%s) ",VPI2_VERSION,PPSC_H_VERSION);
}

#define	j44(a,b)	((a&0xf0)|((b>>4)&0x0f))

#define CST(v)	w2(0xc);w0(v);w2(4);w2(6);w2(4);w2(0xc);
#define DST(v)	w2(0xc);w0(v);w2(0xc);w2(0xe);w2(0xc);w2(4);w2(0xc);

static inline int imm_cpp (PHA *pha, unsigned char b)
{
	unsigned char s1, s2, s3;

	w2(0xc);
	udelay(2);                  /* 1 usec - infinite */
	w0(0xaa);
	udelay(10);                 /* 7 usec - infinite */
	w0(0x55);
	udelay(10);                 /* 7 usec - infinite */
	w0(0x00);
	udelay(10);                 /* 7 usec - infinite */
	w0(0xff);
	udelay(10);                 /* 7 usec - infinite */
	s1 = r1() & 0xb8;
	w0(0x87);
	udelay(10);                 /* 7 usec - infinite */
	s2 = r1() & 0xb8;
	w0(0x78);
	udelay(10);
	s3 = r1() & 0x38;
	/*
	 * Values for b are:
	 * 0000 00aa    Assign address aa to current device
	 * 0010 00aa    Select device aa in EPP Winbond mode
	 * 0010 10aa    Select device aa in EPP mode
	 * 0011 xxxx    Deselect all devices
	 * 0110 00aa    Test device aa
	 * 1101 00aa    Select device aa in ECP mode
	 * 1110 00aa    Select device aa in Compatible mode
	 */
	w0(b);
	udelay(2);                  /* 1 usec - infinite */
	w2(0x0c);
	udelay(10);                 /* 7 usec - infinite */
	w2(0x0d);
	udelay(2);                  /* 1 usec - infinite */
	w2(0x0c);
	udelay(10);                 /* 7 usec - infinite */
	w0(0xff);
	udelay(10);                 /* 7 usec - infinite */

	/*
	 * The following table is electrical pin values.
	 * (BSY is inverted at the CTR register)
	 *
	 *       BSY  ACK  POut SEL  Fault
	 * S1    0    X    1    1    1
	 * S2    1    X    0    1    1
	 * S3    L    X    1    1    S
	 *
	 * L => Last device in chain
	 * S => Selected
	 *
	 * Observered values for S1,S2,S3 are:
	 * Disconnect => f8/58/78
	 * Connect    => f8/58/70
	 */
	if ((s1 == 0xb8) && (s2 == 0x18) && (s3 == 0x30))
		return 1;               /* Connected */
	if ((s1 == 0xb8) && (s2 == 0x18) && (s3 == 0x38))
		return 0;               /* Disconnected */

	return -1;                  /* No device present */
}

static inline int do_vpi2_connect (PHA *pha)
{
	pha->saved_r0 = r0();
	pha->saved_r2 = r2();

	imm_cpp(pha, 0xe0); /* Select device 0 in compatible mode */
	imm_cpp(pha, 0x30); /* Disconnect all devices */

	if (pha->mode >= 2)
		/* Select device 0 in EPP mode */
		return imm_cpp (pha, 0x28);

	/* Select device 0 in compatible mode */
	return imm_cpp (pha, 0xe0);
}

static void vpi2_connect (PHA *pha)
{
	printk ("--> vpi2_connect\n");
	do_vpi2_connect (pha);
	printk ("<--\n");
}

static void vpi2_disconnect (PHA *pha)
{
	printk ("--> vpi2_disconnect\n");
	imm_cpp (pha, 0x30); /* Disconnect all devices */

	w2(pha->saved_r2);
	w0(pha->saved_r0);
	printk ("<--\n");
}


/* There are no data-transfer tests available, just this simple 
   check that we are talking to a VPI2.  */
static int vpi2_test_proto (PHA *pha)
{
	int	e = 1;

	printk ("--> vpi2_test_proto\n");
	if (do_vpi2_connect(pha) == 1)
		e--;

	vpi2_disconnect (pha);
	printk ("<-- %d\n", e);
	return e;
}

static int vpi2_select (PHA *pha, int initiator, int target)
{
	printk ("--> vpi2_select\n");
	w2(0xc);
	if (r1() & 0x08) {
		printk ("<-- -1 (busy)\n");
		return -1;	/* bus busy */
	}

	/*
	 * Now assert the SCSI ID (HOST and TARGET) on the data bus
	 */
	w2(0x4);
	w0(0x80 | (1 << target));
	udelay (1);

	/*
	 * Deassert SELIN first followed by STROBE
	 */
	w2(0xc);
	w2(0xd);

	printk ("<-- 0\n");
	return 0;

}

static int vpi2_test_select (PHA *pha)
{
	int val = r1() & 0x08;
	printk ("--> vpi2_test_select\n<-- %d\n", val);
	return val;	/* BSY asserted ? */
}

static void vpi2_select_finish (PHA *pha)
{
	printk ("--> vpi2_select_finish\n<--\n");
	w2(0xc);
}

static void vpi2_deselect (PHA *pha)
{
	printk ("--> vpi2_deselect\n<--\n");
	w2(0xc);
}

static int vpi2_get_bus_status (PHA *pha)
{
	int val;
	printk ("--> vpi2_get_bus_status\n");
	w2(0xc);
	val = vpi2_map[r1()];
	printk ("<-- %d\n", val);
	return val;
}

/* These functions are inlined so the C optimiser can move the switches
   outside of loops where possible, am I dreaming ?  */

static inline int vpi2_read (PHA *pha, int first)
{
	int l, h;

	printk ("--> vpi2_read\n<--\n");

	switch (pha->mode) {

	case 0:	if (first) w2(4);
		w2(0x6); h = r1();
		w2(0x5); l = r1(); w2(4);
		return j44(h,l);

	case 1: if (first) w2(0x25);
		w2(0x26);
		l = r0();
		w2(0x25);
		return l;

	case 2: if (first) w2(0x24);
		return r4();

	default: return -1;

	}
}

static inline void vpi2_write (PHA *pha, int v, int first )
{
	static int alternate;

	printk ("--> vpi2_write\n");

	switch (pha->mode) {

	case 0:
	case 1:
		if (first) {
			w2(0xc);
			alternate = 0;
		}
		w0(v);
		if (alternate)
			w2(0x0);
		else
			w2(0x5);
		alternate = 1 - alternate;
		break;

	case 2:	if (first) w2(0x4);
		w4(v);
		break;

	}
	printk ("<--\n");
}

static void vpi2_slow_start (PHA *pha, char *val)
{
	int r;

	printk ("--> vpi2_slow_start\n");

	w2(0xc);

	r = (r1() & 0x10);

	if (r) *val = vpi2_read(pha,1);
	  else vpi2_write(pha,*val,1);

	printk ("<--\n");
}

static int vpi2_slow_done (PHA *pha)
{
	printk ("--> vpi2_slow_done\n<--\n");
	return 1;  /* vpi2 does its own REQ/ACK handshaking */
}

static void vpi2_slow_end (PHA *pha)
{
	printk ("--> vpi2_slow_end\n<--\n");
	w2(0xc);
}

static void vpi2_start_block (PHA *pha, int rd)
{
	printk ("--> vpi2_start_block\n<--\n");
	pha->priv_flag = rd;
}

static int vpi2_transfer_ready (PHA *pha)
{
	int b;

	printk ("--> vpi2_transfer_ready\n<--\n");
	b = vpi2_get_bus_status(pha);
	if ((b & PPSC_PH_STAT) == PPSC_PH_STAT) return -1;
	if (b == (PPSC_REQ|PPSC_BSY| pha->priv_flag)) return 128;
	return 0;
}

static int vpi2_transfer_block (PHA *pha, char * buf, int buflen, int rd)
{
	int k, n, i;

	printk ("--> vpi2_transfer_block\n");
	k = 0;
	while (k < buflen) {
	    n = vpi2_transfer_ready(pha);
	    if (n <= 0 ) break;
	    if (n > (buflen-k)) n = buflen-k;
	    for (i=0;i<n;i++) 
		if (rd) buf[k++] = vpi2_read(pha,!i);
	          else vpi2_write(pha,buf[k++],!i);
	    w2(0xc);
	}
	printk ("<-- %d\n", k);
	return k;
}

static int vpi2_transfer_done (PHA *pha)
{
	printk ("--> vpi2_transfer_done\n<-- 1\n");
	return 1;
}

static void vpi2_end_block (PHA *pha, int rd)
{
	printk ("--> vpi2_end_block\n<--\n");
	w2(0xc);
}

static void vpi2_reset_bus (PHA *pha)
{
	printk ("--> vpi2_reset_bus\n<--\n");
	w2(0xc);
	w0(0x40); w2(8); udelay(60);
	w2(0xc); 
}

/* Make these correspond to the actual modes supported by the adapter */

static char *(mode_strings[3]) = {"Nybble","PS/2","EPP"};

static struct ppsc_protocol vpi2_psp =  {

 	{&host0,&host1,&host2,&host3}, 		/* params        */
	&host_structs,				/* hosts         */
	3,					/* num_modes     */
	2,					/* epp_first     */
	1,					/* default_delay */
	0,					/* can_message   */
	16,					/* sg_tablesize  */
	mode_strings,
	vpi2_init,
	NULL,
	vpi2_connect,
	vpi2_disconnect,
	vpi2_test_proto,
	vpi2_select,
	vpi2_test_select,
	vpi2_select_finish,
	vpi2_deselect,
	vpi2_get_bus_status,
	vpi2_slow_start,
	vpi2_slow_done,
	vpi2_slow_end,
	vpi2_start_block,
	vpi2_transfer_block,
	vpi2_transfer_ready,
	vpi2_transfer_done,
	vpi2_end_block,
	vpi2_reset_bus
};

int vpi2_detect (struct scsi_host_template *tpnt)
{
	int val;
	printk ("--> vpi2_detect\n");
	val = ppsc_detect( &vpi2_psp, tpnt, verbose);
	printk ("<-- %d\n", val);
	return val;
}

#ifdef MODULE

struct scsi_host_template driver_template = PPSC_TEMPLATE(vpi2);

#include "scsi_module.c"

MODULE_LICENSE("GPL");

#else

void vpi2_setup (char *str, int *ints)
{
	printk ("--> vpi2_setup\n");
	ppsc_gen_setup(stt,4,str);
	printk ("<--\n");
}

#endif

/* end of vpi2.c */

