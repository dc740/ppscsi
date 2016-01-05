#ifndef _PPSC_H
#define _PPSC_H

/*
	ppscsi.h	(c) 1999 Grant Guenther <grant@torque.net>
			Under the terms of the GNU public license.

        This header file defines a common interface for constructing
        low-level SCSI drivers for parallel port SCSI adapters.

*/

#define	PPSC_H_VERSION	"0.92"

#include <linux/module.h>
#include <generated/autoconf.h>
#include <linux/version.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <asm/io.h>
#include <linux/blkdev.h>

#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>


/* ppscsi global functions */

extern void ppsc_make_map( char map[256], char key[5], int inv);

extern int ppsc_proc_info(struct Scsi_Host *, char *,char **,off_t,int,int);
extern int ppsc_command(struct scsi_cmnd *);
extern int ppsc_queuecommand(struct Scsi_Host *shost, struct scsi_cmnd *cmd);
extern int ppsc_abort(struct scsi_cmnd *);
extern int ppsc_reset(struct scsi_cmnd *);
extern int ppsc_biosparam(struct scsi_device *, struct block_device *, sector_t capacity, int[]);
extern int ppsc_release(struct Scsi_Host *);

#ifndef PPSC_BASE

/* imports for hosts.c */

#ifdef CONFIG_PPSCSI_T348
extern int t348_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_T358
extern int t358_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_ONSCSI
extern int onscsi_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_EPST
extern int epst_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_EPSA2
extern int epsa2_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_VPI0
extern int vpi0_detect( struct scsi_host_template *);
#endif

#ifdef CONFIG_PPSCSI_SPARCSI
extern int sparcsi_detect( struct scsi_host_template *);
#endif

#endif
 
#define PPSC_TEMPLATE(proto){			   \
	.name =			#proto,	   	   \
        .detect =         	proto##_detect,    \
	.release =		ppsc_release,      \
	.proc_name =		#proto,		   \
        .queuecommand =   	ppsc_queuecommand, \
	.eh_abort_handler =	ppsc_abort,	   \
	.eh_bus_reset_handler =   ppsc_reset,	   \
	.eh_host_reset_handler =  ppsc_reset,	   \
        .bios_param =     	ppsc_biosparam,    \
        .can_queue =      	1,         	   \
        .sg_tablesize =   	SG_NONE,           \
        .cmd_per_lun =    	1,                 \
        .use_clustering = 	DISABLE_CLUSTERING \
}

/* types used by the actual driver modules */

#ifdef PPSC_BASE

#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>


struct setup_tab_t {

        char    *tag;   /* variable name */
        int     size;   /* number of elements in array */
        int     *iv;    /* pointer to variable */
};

typedef struct setup_tab_t STT;

extern void ppsc_gen_setup( STT t[], int n, char *ss );

typedef struct ppsc_host_adapter PHA;

struct ppsc_host_adapter {

	char	ident[80];		/* Adapter name and version info */

	char	device[12];		/* device name for messages */

	struct Scsi_Host *host_ptr;	/* SCSI host structure */
	struct ppsc_protocol *proto;	/* adapter protocol */

	int	port;			/* parallel port base address */
	int 	mode;			/* transfer mode in use */
	int	delay;  		/* parallel port settling delay */
	int     saved_r0;		/* saved port state */
	int	saved_r2;		/* saved port state */

	int	reserved;		/* number of ports reserved */
	int     tmo;			/* default command timeout */
	int	verbose;		/* logging level */
	int	quiet;			/* do not log PPSC_FAIL msgs */ 

	int	slow_targets;		/* bit mask for disabling block mode */

	wait_queue_head_t parq;		/* semaphore for parport sharing */
	struct pardevice *pardev;	/* pointer to pardevice */
	const char *parname;		/* parport name */
	int	claimed;		/* parport has been claimed */
	void	(*claim_cont)(PHA *);   /* continuation for parport wait */

	void 	(*continuation)(PHA *); /* next "interrupt" handler */
	int  	(*ready)(PHA *);	/* current ready test */
	unsigned long  	then;		/* jiffies at start of last wait */
	unsigned long  	timeout;	/* when to timeout this wait */
	int	timedout;		/* timeout was seen */
	int  	timer_active;		/* we're using a timer */
	int  	wq_active;		/* we have a task queued */
	int	nice;			/* tune the CPU load */
	struct timer_list timer;	/* timer queue element */
	struct work_struct wq;		/* task queue element */

	int	private[8];		/* for the protocol layer, if needed */
	char	*priv_ptr;
	int	priv_flag;

	struct scsi_cmnd *cur_cmd;		/* current command on this host */
	void  (*done)(struct scsi_cmnd *);	/* current "done" function */

	int	overflow;		/* excess bytes transferred */
	int	bulk;			/* should we use block mode ? */
	int	tlen;			/* total transfer length */
	int	abort_flag;		/* abort=1 reset=2 requested */
	int	return_code;		/* build return value here */

	struct scatterlist *sg_list;	/* current fragment, if any */
	int 	sg_count;		/* remaining fragments */
	char	*cur_buf;		/* current buffer pointer */
	int	cur_len;		/* remaining bytes in buffer */

	struct timer_list sleeper;	/* for BUSY handling */

	int	last_phase;		/* to detect phase changes */
	char	message_byte;
	char	status_byte;

	int	cmd_count;		/* bytes of command transfered */
	int	data_count;		/* bytes of data transferred */
	int	data_dir;		/* direction of transfer */

	int	tot_cmds;		/* number of commands processed */
	long	tot_bytes;		/* total bytes transferred */
	int	tot_errs;		/* number of failed commands */

	int	protocol_error;		/* Some protocols can set this
					   != zero to signal a fatal error
					   we report it and expect to die
					*/
};

/* constants for 'verbose' */

#define PPSC_VERB_NORMAL 0
#define PPSC_VERB_PROBE  1
#define PPSC_VERB_TRACE  2
#define PPSC_VERB_DEBUG  3
#define PPSC_VERB_FULL   4

#define V_PROBE	(pha->verbose >= PPSC_VERB_PROBE)
#define V_TRACE	(pha->verbose >= PPSC_VERB_TRACE)
#define V_DEBUG	(pha->verbose >= PPSC_VERB_DEBUG)
#define V_FULL	(pha->verbose >= PPSC_VERB_FULL)

/* constants for abort_flag */

#define	PPSC_DO_ABORT	1
#define PPSC_DO_RESET	2


struct ppsc_protocol {

	int	(*params[4])[8];	/* hostN tuning parameters */

	PHA     (*hosts)[4];		/* actual PHA structs */

        int     num_modes;      	/* number of modes*/
        int     epp_first;      	/* modes >= this use 8 ports */
        int     default_delay;  	/* delay parameter if not specified */

	int	can_message;		/* adapter can send/rcv SCSI msgs */
	int	default_sg_tablesize;	/* sg_tablesize if not specified */

	char	**mode_names;		/* printable names of comm. modes */

/* first two functions are NOT called with the port claimed. */

	void (*init)(PHA *);		/* (pha)
					   protocol initialisation 
				           should fill in pha->ident */
	void (*release)(PHA *);		/* (pha)  optional
					   protocol no longer in use */
	void (*connect)(PHA *);		/* (pha)
					   connect to adapter */
	void (*disconnect)(PHA *);	/* (pha)
					   release adapter */
	int (*test_proto)(PHA *);	/* (pha)   optional
					   test protocol in current settings,
					   returns error count */
	int (*select)(PHA *,int,int);   /* (pha,initiator,target)
					   start artibration and selection
					   0 = OK, -1 = arb. failed */
	int (*test_select)(PHA *);	/* (pha)
					   test for selection to complete
					   1 = OK, 0 try again */
	void (*select_finish)(PHA *);	/* (pha) optional
					   called after successful select */
	void (*deselect)(PHA *);	/* (pha)
					   release SCSI bus */
	int (*get_bus_status)(PHA *);	/* (pha)
					   return (REQ,BSY,MSG,C/D,I/O) */
	void (*slow_start)(PHA *,char *); /* (pha,byte)
					   start transfer of one byte using
					   explicit handshaking */
	int (*slow_done)(PHA *);	/* (pha)
					   has the device acked the byte ? */
	void (*slow_end)(PHA *);	/* (pha)
					   shut down the slow transfer */
	void (*start_block)(PHA *,int);	/* (pha,read) 
					   start data transfer */
	int (*transfer_block)(PHA *,char *,int,int); 
					/* (pha,buf,len,read)
					   transfer as much as possible and
					   return count of bytes  
					   can return -1 if error detected */
	int (*transfer_ready)(PHA *pha);/* (pha)
					   can we go again yet ? 
					   >0 = yes, 0 = try again, -1 = done */
	int (*transfer_done)(PHA *pha); /* (pha)
					   has all data been flushed ? 
					   1 = yes, 0 = try again */
	void (*end_block)(PHA *,int);	/* (pha,read) 
					   shut down block transfer */
	void (*reset_bus)(PHA *);	/* (pha) optional
					   reset SCSI bus if possible */

};

/* constants for the params array */

#define PPSC_PARM_PORT	0
#define PPSC_PARM_MODE	1
#define PPSC_PARM_DLY	2
#define PPSC_PARM_NICE	3
#define PPSC_PARM_SGTS  4
#define PPSC_PARM_SLOW  5

/* constants for get_bus_status */

#define	PPSC_REQ	16
#define PPSC_BSY	8	
#define PPSC_MSG	4
#define PPSC_CD		2
#define PPSC_IO		1

/* phases */

#define PPSC_PH_NONE	0
#define PPSC_PH_WRITE	(PPSC_REQ|PPSC_BSY)
#define PPSC_PH_READ	(PPSC_PH_WRITE|PPSC_IO)
#define PPSC_PH_CMD	(PPSC_PH_WRITE|PPSC_CD)
#define PPSC_PH_STAT	(PPSC_PH_READ|PPSC_CD)
#define PPSC_PH_MSGIN	(PPSC_PH_STAT|PPSC_MSG)

typedef struct ppsc_protocol PSP;

extern int ppsc_detect( PSP *, struct scsi_host_template *, int);

#ifdef PPSC_HA_MODULE

static int verbose = PPSC_VERB_NORMAL;

static int host0[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
static int host1[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
static int host2[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
static int host3[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

#ifndef MODULE

static STT stt[4] = { {"host0",8,host0},
		      {"host1",8,host1},
		      {"host2",8,host2},
		      {"host3",8,host3} };
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,10)
MODULE_PARM(host0,"1-8i");
MODULE_PARM(host1,"1-8i");
MODULE_PARM(host2,"1-8i");
MODULE_PARM(host3,"1-8i");
MODULE_PARM(verbose,"i");
#else
module_param_array(host0, int, NULL, 0);
module_param_array(host1, int, NULL, 0);
module_param_array(host2, int, NULL, 0);
module_param_array(host3, int, NULL, 0);
module_param(verbose, int, 0);
#endif

static struct ppsc_host_adapter host_structs[4];

#define delay_p                 (pha->delay?udelay(pha->delay):0)
#define out_p(offs,byte)        outb(byte,pha->port+offs); delay_p;
#define in_p(offs)              (delay_p,inb(pha->port+offs))

#define w0(byte)                do {out_p(0,byte);} while (0)
#define r0()                    (in_p(0) & 0xff)
#define w1(byte)                do {out_p(1,byte);} while (0)
#define r1()                    (in_p(1) & 0xff)
#define w2(byte)                do {out_p(2,byte);} while (0)
#define r2()                    (in_p(2) & 0xff)
#define w3(byte)                do {out_p(3,byte);} while (0)
#define w4(byte)                do {out_p(4,byte);} while (0)
#define r4()                    (in_p(4) & 0xff)
#define w4w(data)               do {outw(data,pha->port+4); delay_p;} while (0)
#define w4l(data)               do {outl(data,pha->port+4); delay_p;} while (0)
#define r4w()                   (delay_p,inw(pha->port+4)&0xffff)
#define r4l()                   (delay_p,inl(pha->port+4)&0xffffffff)

#endif  /* PPSC_HA_MODULE */
#endif  /* PPSC_BASE */
#endif  /* _PPSC_H */

/* end of ppscsi.h */


