/*
        ppscsi.c        (C) 1999 Grant Guenther <grant@torque.net>
                        (C) 2000 Tim Waugh <tim@cyberelk.demon.co.uk>
                        Under the terms of the GNU general public license.

	This is the common code shared by the PPSCSI family of
	low-level drivers for parallel port SCSI host adapters.


	To use one of the ppSCSI drivers, you must first have this module
	built-in to your kernel, or loaded.  Then, you can load the
	appropriate protocol module.  All protocol modules accept the
	same parameters:

	verbose=N	determines the logging level where N= 
			  0   only serious errors are logged
			  1   report progress messages while probing adapters
			  2   log the scsi commands sent to adapters
			  3   basic debugging information 
			  4   full debugging (generates lots of output)

	hostN=<port>,<mode>,<dly>,<nice>,<sgts>,<slow>

			sets per-host-adapter parameters where

			N 	is between 0 and 3, each protocol can 
                        	support up to four separate adapters.

			<port>	The parport for this adapter, eg:
				0 for parport0.

			<mode>	Protocol dependent mode number.  Usually
				probed to determine the fastest available
				mode.

			<dly>   microseconds of delay per port access.
				Default is protocol dependent.

			<nice>  Determines this host's ability to load
				the system.  Default 0.  Set to 1 or 2
				to reduce load at the expense of device
				performance.

			<sgts>	scatter-gather table size.

			<slow>  bit mask of targets on which to force
				all commands to use explicit REQ/ACK
				handshaking, rather than adapter buffers.

*/

#define PPSC_VERSION	"0.92"

#define PPSC_BASE
#include "ppscsi.h"
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/ioport.h>

#include <linux/parport.h>

#define PPSC_GEN_TMO	40*HZ
#define PPSC_SELECT_TMO HZ/10
#define PPSC_PROBE_TMO  HZ/2
#define PPSC_RESET_TMO	4*HZ
#define PPSC_SLOW_LOOPS	30
#define PPSC_BUSY_SNOOZE HZ;

#define PPSC_DEF_NICE	0
#define PPSC_INITIATOR  7

DEFINE_SPINLOCK(ppsc_spinlock);

static char	ppsc_bulk_map[256];

struct ppsc_port_list_struct {
	struct parport *port;
	struct ppsc_port_list_struct *next;
};
static struct ppsc_port_list_struct *ppsc_port_list;

/* ppsc_attach and ppsc_detach are for keeping a list of currently
 * available ports, held under a mutex.  We do this rather than
 * using parport_enumerate because it stops a load of races.
 */

static void ppsc_attach (struct parport *port)
{
	struct ppsc_port_list_struct *add;

	add = kmalloc (sizeof (struct ppsc_port_list_struct), GFP_KERNEL);
	if (!add) {
		printk (KERN_WARNING "ppscsi: memory squeeze\n");
		return;
	}

	atomic_inc(&port->ref_count);
	add->port = port;
	add->next = ppsc_port_list;
	wmb ();
	ppsc_port_list = add;
}

static void ppsc_detach (struct parport *port)
{
	/* Do nothing.  We have a reference to the port already, so
	 * it won't go away.  We'll clean up the port list when we
	 * unload. */
}

static struct parport_driver ppsc_driver = {
	name:	"ppscsi",
	attach:	ppsc_attach,
	detach:	ppsc_detach
};

void ppsc_make_map (char map[256], char key[5], int inv)
{
	int i, j;

	for (i=0;i<256;i++) {
		map[i] = 0;
		for (j=0;j<5;j++)
			map[i] = (map[i] << 1) | ((i & key[j]) != inv*key[j]);
	}
}

void ppsc_gen_setup (STT t[], int n, char *ss)
{
	int j, k, sgn;

	k = 0;
	for (j=0;j<n;j++) {
		k = strlen(t[j].tag);
		if (strncmp(ss,t[j].tag,k) == 0) break;
	}
	if (j == n) return;

	if (ss[k] == 0) {
		t[j].iv[0] = 1;
		return;
	}

	if (ss[k] != '=') return;
	ss += (k+1);

	k = 0;
	while (ss && (k < t[j].size)) {
		if (!*ss) break;
		sgn = 1;
		if (*ss == '-') { ss++; sgn = -1; }
		if (!*ss) break;
		if (isdigit(*ss))
			t[j].iv[k] = sgn * simple_strtoul(ss,NULL,0);
		k++; 
		if ((ss = strchr(ss,',')) != NULL) ss++;
	}
}

static void ppsc_set_intr (PHA *pha, void (*continuation)(PHA *), 
			   int (*ready)(PHA *), int timeout)
{
	unsigned long flags;

	spin_lock_irqsave(&ppsc_spinlock,flags);

	pha->continuation = continuation;
	pha->ready = ready;
	if (timeout) 
		pha->timeout = jiffies + timeout;
	  else	pha->timeout = pha->then + pha->tmo;

	if (!pha->nice && !pha->wq_active) {
#ifdef HAVE_DISABLE_HLT
		disable_hlt();
#endif
		pha->wq_active = 1;
		schedule_work (&pha->wq);
	}

	if (!pha->timer_active) {
		pha->timer_active = 1;
		pha->timer.expires = jiffies + ((pha->nice>0)?(pha->nice-1):0);
		add_timer(&pha->timer);
	}

	spin_unlock_irqrestore(&ppsc_spinlock,flags);
}

static void ppsc_tq_int (struct work_struct *data)
{
	void (*con)(PHA *);
	unsigned long flags;
	PHA *pha = (PHA *)data;

	spin_lock_irqsave(&ppsc_spinlock,flags);

	con = pha->continuation;

#ifdef HAVE_DISABLE_HLT
	enable_hlt();
#endif

	pha->wq_active = 0;

	if (!con) {
		spin_unlock_irqrestore(&ppsc_spinlock,flags);
		return;
	}
	pha->timedout = time_after_eq (jiffies, pha->timeout);
	if (!pha->ready || pha->ready(pha) || pha->timedout) {
		pha->continuation = NULL;
		spin_unlock_irqrestore(&ppsc_spinlock,flags);
		con(pha);
		return;
	}

#ifdef HAVE_DISABLE_HLT
	disable_hlt();
#endif

	pha->wq_active = 1;
	schedule_work (&pha->wq);
	spin_unlock_irqrestore(&ppsc_spinlock,flags);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,00)
static void ppsc_timer_int (unsigned long data) {
	PHA *pha = (PHA *)data;
#else
static void ppsc_timer_int (struct timer_list *t){
	PHA *pha = from_timer(pha, t, timer);
#endif
	void (*con)(PHA *);
	unsigned long flags;


	spin_lock_irqsave(&ppsc_spinlock,flags);

	con = pha->continuation;
	pha->timer_active = 0;
	if (!con) {
		spin_unlock_irqrestore(&ppsc_spinlock,flags);
		return;
	}
	pha->timedout = time_after_eq (jiffies, pha->timeout);
	if (!pha->ready || pha->ready(pha) || pha->timedout) {
		pha->continuation = NULL;
		spin_unlock_irqrestore(&ppsc_spinlock,flags);
		con(pha);
		return;
	}
	pha->timer_active = 1;
	pha->timer.expires = jiffies + ((pha->nice>0)?(pha->nice-1):0);
	add_timer(&pha->timer);
	spin_unlock_irqrestore(&ppsc_spinlock,flags);
}

static void ppsc_wake_up( void *p)
{
	PHA *pha = (PHA *) p;
	unsigned long flags;
	void (*cont)(PHA *) = NULL;

	spin_lock_irqsave(&ppsc_spinlock,flags);

	if (pha->claim_cont && 
	    !parport_claim(pha->pardev)) {
		cont = pha->claim_cont;
		pha->claim_cont = NULL;
		pha->claimed = 1;
	}

	spin_unlock_irqrestore(&ppsc_spinlock,flags);	  

	wake_up(&(pha->parq));

	if (cont) cont(pha);
}

void ppsc_do_claimed (PHA *pha, void(*cont)(PHA *))
{
	unsigned long flags;

	spin_lock_irqsave(&ppsc_spinlock,flags); 

	if (!parport_claim(pha->pardev)) {
		pha->claimed = 1;
		spin_unlock_irqrestore(&ppsc_spinlock,flags);
		cont(pha);
	} else {
		pha->claim_cont = cont;
		spin_unlock_irqrestore(&ppsc_spinlock,flags);	  
	}
}

static void ppsc_claim (PHA *pha)
{
	if (pha->claimed) return;
	pha->claimed = 1;

	wait_event (pha->parq, !parport_claim (pha->pardev));
}

static void ppsc_unclaim (PHA *pha)
{
	pha->claimed = 0;
	parport_release(pha->pardev);
}

static void ppsc_unregister_parport (PHA *pha)
{
	parport_unregister_device(pha->pardev);
	pha->pardev = NULL;
}

static int ppsc_register_parport (PHA *pha, int verbose)
{
	struct ppsc_port_list_struct *ports;
	struct parport *port = NULL;

	ports = ppsc_port_list;
	while((ports)&&(ports->port->number != pha->port))
		ports = ports->next;
	if (ports) {
		port = ports->port;
		pha->pardev = parport_register_device(port, pha->device,
						      NULL, ppsc_wake_up, NULL,
						      0, (void *)pha);
	} else {
		printk (KERN_DEBUG "%s: no such device: parport%d\n",
			pha->device, pha->port);
		return 1;
	}

	if (!pha->pardev) {
		printk (KERN_DEBUG "%s: couldn't register device\n",
			pha->device);
		return 1;
	}

	init_waitqueue_head (&pha->parq);

	/* For now, cache the port base address.  Won't need this
	   after transition to parport_xxx_yyy. */
	pha->port = port->base;

	if (verbose) 
		printk("%s: 0x%x is %s\n",pha->device,pha->port,
		       port->name);
	pha->parname = port->name;
	return 0;
}

/* Here's the actual core SCSI stuff ... */

#define PPSC_FAIL(err,msg)  { ppsc_fail_command(pha,err,msg); return; }

static void ppsc_start (PHA *pha);
static void ppsc_select_intr (PHA *pha);
static void ppsc_engine (PHA *pha);
static void ppsc_transfer (PHA *pha);
static void ppsc_transfer_done (PHA *pha);
static int ppsc_slow (PHA *pha, char *val);
static void ppsc_slow_done (PHA *pha);
static void ppsc_cleanup (PHA *pha);
static void ppsc_fail_command (PHA *pha, int err_code, char *msg);
static int ppsc_ready (PHA *pha);

/* synchronous interface is deprecated, but we maintain it for 
   internal use.  It just starts an asynchronous command and waits
   for it to complete.
*/

int ppsc_command (struct scsi_cmnd *cmd)
{
	PHA *pha = (PHA *) cmd->device->host->hostdata[0];

	pha->cur_cmd = cmd;
	pha->done = NULL;
	pha->then = jiffies;

	ppsc_do_claimed(pha,ppsc_start);

	while (pha->cur_cmd) ssleep(1);

	return cmd->result;
}

int ppsc_queuecommand_lck (struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *))
{
	PHA *pha = (PHA *) cmd->device->host->hostdata[0];

	if (pha->cur_cmd) {
		printk("%s: Driver is busy\n",pha->device);
		return 0;
	}

	pha->cur_cmd = cmd;
	pha->done = cmd->scsi_done;
	pha->then = jiffies;

	ppsc_do_claimed(pha,ppsc_start);

	return 0;
}
DEF_SCSI_QCMD(ppsc_queuecommand)


static void ppsc_arb_fail (PHA *pha)
{
	PPSC_FAIL(DID_BUS_BUSY,"Arbitration failure");
}

static void ppsc_start (PHA *pha)
{
	int k, r, b, bf; 
	struct scatterlist *p;

	pha->last_phase = PPSC_PH_NONE;
	pha->return_code = (DID_OK << 16);
	pha->overflow = 0;
	pha->protocol_error = 0;
	pha->cmd_count = 0;

	k = pha->cur_cmd->cmnd[0];
	bf = ppsc_bulk_map[k];

	bf &= (!((1<<pha->cur_cmd->device->id) & pha->slow_targets));
	
	#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
		r = pha->cur_cmd->sdb.table.nents;
	#else
		r = pha->cur_cmd->use_sg;
	#endif
	if (r) {
		b = 0;
		#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
			p = (struct scatterlist *)pha->cur_cmd->sdb.table.sgl;
		#else
			p = (struct scatterlist *)pha->cur_cmd->request_buffer;
		#endif
		for (k=0;k<r;k++) {
			b += p->length;
			p++;
		}
	} else { 
		#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
			b = pha->cur_cmd->sdb.length;
		#else
			b = pha->cur_cmd->request_bufflen;
		#endif
	}

	bf &= (b > 127);

	if (V_DEBUG)
		printk("%s: Target %d, bl=%d us=%d bf=%d cm=%x\n",
		       pha->device,pha->cur_cmd->device->id,b,r,bf,k);

	pha->bulk = bf;
	pha->tlen = b;

	pha->proto->connect(pha);

	r = 0;
	while (r++ < 5) {
		k = pha->proto->select(pha,PPSC_INITIATOR,pha->cur_cmd->device->id);
		if (k != -1) break;
		udelay(200);
	}

	if (k == -1) { 
		ppsc_set_intr(pha,ppsc_arb_fail,NULL,1);
		return;
	}

	ppsc_set_intr(pha,ppsc_select_intr,pha->proto->test_select,
		      PPSC_SELECT_TMO);
}

static void ppsc_select_intr (PHA *pha)
{
	if (!pha->proto->test_select(pha)) {
		pha->return_code = DID_NO_CONNECT << 16;
		ppsc_cleanup(pha);
		return;
	}
	if (pha->proto->select_finish) 
		pha->proto->select_finish(pha);

	if (V_FULL) 
		printk("%s: selected target\n",pha->device);
	
	pha->timedout = 0;
	ppsc_engine(pha);
}

static void ppsc_update_sg (PHA *pha)
{
	if ((!pha->cur_len) && pha->sg_count) {
		pha->sg_count--;
		pha->sg_list++;
		pha->cur_buf = page_address(sg_page(pha->sg_list)) + pha->sg_list->offset;
		pha->cur_len = pha->sg_list->length;
	}
}

static void ppsc_engine (PHA *pha)
{
	int phase, i;
	char *sb;

	while (1) {
		if ((pha->last_phase == PPSC_PH_MSGIN) || 
		    ((pha->last_phase == PPSC_PH_STAT) 
		     && (!pha->proto->can_message))) {
			pha->return_code |= (pha->status_byte & STATUS_MASK)
				|  (pha->message_byte << 8);
			ppsc_cleanup(pha);
			return;
		}

		phase = pha->proto->get_bus_status(pha);

		if (pha->abort_flag) 
			PPSC_FAIL(DID_ABORT,"Command aborted");

		if (pha->protocol_error)
			PPSC_FAIL(DID_ERROR,"Adapter protocol failure");

		if (!(phase & PPSC_BSY)) {
			if (pha->last_phase == PPSC_PH_STAT) {
				if (V_DEBUG) printk("%s: No msg phase ?\n", pha->device);
				pha->return_code |= (pha->status_byte & STATUS_MASK);
				ppsc_cleanup(pha);
				return;
			}
			PPSC_FAIL(DID_ERROR,"Unexpected bus free");
		}

		if (!(phase & PPSC_REQ)) {
			if (pha->timedout) 
				PPSC_FAIL(DID_TIME_OUT,"Pseudo-interrupt timeout");
			ppsc_set_intr(pha,ppsc_engine,ppsc_ready,0);
			return;
		} 

		switch (phase) {

		case PPSC_PH_CMD:

			if (phase != pha->last_phase) {
				if (pha->last_phase != PPSC_PH_NONE) 
					PPSC_FAIL(DID_ERROR,"Phase sequence error 1");
				pha->cmd_count = 0;
				if (V_TRACE) {
					printk("%s: Command to %d (%d): ",
					       pha->device, pha->cur_cmd->device->id,
					       pha->cur_cmd->cmd_len);
					for (i=0;i<pha->cur_cmd->cmd_len;i++)
						printk("%2x ",pha->cur_cmd->cmnd[i]);
					printk("\n");
				}
			}

			pha->last_phase = phase;

			if (pha->cmd_count >= pha->cur_cmd->cmd_len)
				PPSC_FAIL(DID_ERROR,"Command buffer overrun");

			if (!ppsc_slow(pha,&(pha->cur_cmd->cmnd[pha->cmd_count++])))
				return;

			break;

		case PPSC_PH_READ:
		case PPSC_PH_WRITE:

			if (phase != pha->last_phase) {
				if (pha->last_phase != PPSC_PH_CMD)
					PPSC_FAIL(DID_ERROR,"Phase sequence error 2");
				pha->data_dir = phase & PPSC_IO;
				pha->data_count = 0;

				#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
					pha->sg_count = pha->cur_cmd->sdb.table.nents;
				#else
					pha->sg_count = pha->cur_cmd->use_sg;
				#endif
				if (pha->sg_count) {
					pha->sg_count--;
					pha->sg_list = 
						#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
							(struct scatterlist *)pha->cur_cmd->sdb.table.sgl;
						#else
							(struct scatterlist *)pha->cur_cmd->request_buffer;
						#endif
					pha->cur_buf = page_address(sg_page(pha->sg_list)) + pha->sg_list->offset;
					pha->cur_len = pha->sg_list->length;
				} else {
					#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
						pha->cur_buf = (char *)pha->cur_cmd->sdb.table.sgl;
						pha->cur_len = pha->cur_cmd->sdb.length;
					#else
						pha->cur_buf = pha->cur_cmd->request_buffer;
						pha->cur_len = pha->cur_cmd->request_bufflen;
					#endif
				}

				pha->last_phase = phase;

			}

			if ((pha->bulk) && (pha->cur_len > 0 )) {
				pha->proto->start_block(pha,pha->data_dir);
				ppsc_transfer(pha);
				return;
			}

			ppsc_update_sg(pha);

			if (!pha->cur_len) {
				pha->cur_len = 1;
				pha->cur_buf = (char *)&i;
				i = 0x5a;
				pha->overflow++;
			}

			pha->cur_len--;
			pha->data_count++;

			if (!ppsc_slow(pha,pha->cur_buf++)) return;

			break;

		case PPSC_PH_STAT:

			if ((pha->last_phase != PPSC_PH_CMD) &&
			    (pha->last_phase != PPSC_PH_READ) &&
			    (pha->last_phase != PPSC_PH_WRITE)) 
				PPSC_FAIL(DID_ERROR,"Phase sequence error 3");

			if ((pha->last_phase != PPSC_PH_CMD) &&
			    (V_DEBUG)) {
				printk("%s: %s%s %d bytes\n",
				       pha->device,
				       pha->bulk?"":"slow ",
				       pha->data_dir?"read":"write",
				       pha->data_count);

				if (pha->cur_cmd->cmnd[0] == REQUEST_SENSE) {

					#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
						sb = (char *)pha->cur_cmd->sdb.table.sgl;
					#else
						sb = (char *)pha->cur_cmd->request_buffer;
					#endif
					printk("%s: Sense key: %x ASC: %x ASCQ: %x\n",
					       pha->device, sb[2] & 0xff,
					       sb[12] & 0xff, sb[13] & 0xff);
				}
			}	

			if (pha->overflow) 
				printk("%s: WARNING: data %s overran by %d/%d bytes\n",
				       pha->device,pha->data_dir?"read":"write",
				       pha->overflow,pha->data_count);

			pha->last_phase = phase;

			if (!ppsc_slow(pha,&pha->status_byte)) return;

			break;

		case PPSC_PH_MSGIN:

			if (pha->last_phase != PPSC_PH_STAT)
				PPSC_FAIL(DID_ERROR,"Phase sequence error 4");

			pha->last_phase = phase;
		
			if (V_FULL)
				printk("%s: status = %x\n",pha->device,pha->status_byte);

			if (!ppsc_slow(pha,&pha->message_byte)) return;

			break;

		default:

			PPSC_FAIL(DID_ERROR,"Unexpected bus phase");

		}
	}
}

static void ppsc_transfer (PHA *pha)
{
	int i, j;

	if (pha->timedout) PPSC_FAIL(DID_TIME_OUT,"PDMA timeout"); 

	while(1) {

		if (!(j=pha->proto->transfer_ready(pha))) {
			ppsc_set_intr(pha,ppsc_transfer,
				      pha->proto->transfer_ready,0);
			return;
		}

		if (j < 0) {
			if (V_DEBUG)
				printk("%s: short transfer\n",pha->device);
			ppsc_set_intr(pha,ppsc_transfer_done,
				      pha->proto->transfer_done,0);
			return;
		}

		i = pha->proto->transfer_block(pha,pha->cur_buf,
					       pha->cur_len,pha->data_dir);

		if (V_FULL) printk("%s: Fragment %d\n",pha->device,i); 

		if ((i < 0) || (i > pha->cur_len))
			PPSC_FAIL(DID_ERROR,"Block transfer error");

		pha->cur_len -= i;
		pha->cur_buf += i;
		pha->data_count += i;

		ppsc_update_sg(pha);

		if (pha->cur_len == 0 )  {
			ppsc_set_intr(pha,ppsc_transfer_done,
				      pha->proto->transfer_done,0);
			return;
		}
	}
}

static void ppsc_transfer_done (PHA *pha)
{
	if (pha->timedout) PPSC_FAIL(DID_TIME_OUT,"PDMA done timeout");

	pha->proto->end_block(pha,pha->data_dir);
	ppsc_engine(pha);
}

static int ppsc_slow (PHA *pha, char *val)
{
	int k;	

	pha->proto->slow_start(pha,val);

	k = 0;
	while (k++ < PPSC_SLOW_LOOPS) 
		if (pha->proto->slow_done(pha)) {
			pha->proto->slow_end(pha);
			return 1;
		}

	ppsc_set_intr(pha,ppsc_slow_done,pha->proto->slow_done,0);
	return 0;
}

static void ppsc_slow_done (PHA *pha)
{
	int k;

	if (pha->timedout) PPSC_FAIL(DID_TIME_OUT,"PIO timeout");

	pha->proto->slow_end(pha);
	
	k = 0;
	while (k++ < PPSC_SLOW_LOOPS)
		if (ppsc_ready(pha)) break;
	
	ppsc_engine(pha);
} 
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,00)
static void ppsc_try_again (unsigned long data )
{
	PHA *pha = (PHA *)data;

	ppsc_do_claimed(pha,ppsc_start);
}
#else
static void ppsc_try_again(struct timer_list *t){
	PHA *pha = from_timer(pha, t, sleeper);
	ppsc_do_claimed(pha, ppsc_start);
}
#endif
static void ppsc_cleanup (PHA *pha)
{
	struct scsi_cmnd *cmd;
	void (*done)(struct scsi_cmnd *);
	unsigned long saved_flags;

	pha->tot_bytes += pha->data_count;

	cmd = pha->cur_cmd;
	done = pha->done;
	cmd->result = pha->return_code;
	pha->cur_cmd = 0;

	pha->proto->deselect(pha);
	pha->proto->disconnect(pha);

	if (V_FULL) printk("%s: releasing parport\n",pha->device);

	ppsc_unclaim(pha);

	if (pha->abort_flag) {

		if (V_DEBUG) printk("%s: command aborted !\n",pha->device);

		return;	 /* kill the thread */
	}
	
	if (V_DEBUG) 
		printk("%s: Command status %08x last phase %o\n",
		       pha->device,cmd->result,pha->last_phase);

	if (status_byte(pha->return_code) == BUSY) {

		pha->cur_cmd = cmd;

		if (V_FULL)
			printk("%s: BUSY, sleeping before retry ...\n",
			       pha->device);
		#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,00)
			init_timer (&pha->sleeper);
			pha->sleeper.data = (unsigned long) pha;
			pha->sleeper.function = ppsc_try_again;
		#else
			timer_setup(&pha->sleeper, ppsc_try_again, 0);
		#endif
		pha->sleeper.expires = jiffies + PPSC_BUSY_SNOOZE;
		add_timer(&pha->sleeper);

		return;

	}

	pha->tot_cmds++;

	if ((cmd->cmnd[0] != REQUEST_SENSE) && 
	    (status_byte(pha->return_code) == CHECK_CONDITION)) {

		if (V_FULL) 
			printk("%s: Requesting sense data\n",pha->device);
	
		cmd->cmnd[0] = REQUEST_SENSE;
		cmd->cmnd[1] &= 0xe0;
		cmd->cmnd[2] = 0;
		cmd->cmnd[3] = 0;
		cmd->cmnd[4] = sizeof(cmd->sense_buffer);
		cmd->cmnd[5] = 0;
		cmd->cmd_len = 6;
		#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
			cmd->sdb.table.nents = 0;
			cmd->sdb.table.sgl = (struct scatterlist *) cmd->sense_buffer;
			cmd->sdb.length = sizeof(cmd->sense_buffer);
		#else
			cmd->use_sg = 0;
			cmd->request_buffer = (char *) cmd->sense_buffer;
			cmd->request_bufflen = sizeof(cmd->sense_buffer);
		#endif

		pha->cur_cmd = cmd;
		ppsc_do_claimed(pha,ppsc_start);

		return;
	}

	if (done) { 

		spin_lock_irqsave(pha->host_ptr->host_lock,saved_flags);
		done(cmd);
		spin_unlock_irqrestore(pha->host_ptr->host_lock,saved_flags);

	}

}

static void ppsc_fail_command (PHA *pha, int err_code, char *msg)
{
	int bs;

	pha->tot_errs++;

	bs = pha->proto->get_bus_status(pha);

	pha->return_code = err_code << 16;
	if (!pha->quiet)
		printk("%s: %s, bs=%o cb=%d db=%d bu=%d sg=%d "
		       "rd=%d lp=%o pe=%d cc=%d\n",
		       pha->device, msg, bs,
		       pha->cmd_count, pha->data_count,
		       pha->bulk, pha->sg_count, pha->data_dir,
		       pha->last_phase, pha->protocol_error, pha->tot_cmds);

	ppsc_cleanup(pha);
}

static int ppsc_ready (PHA *pha)
{
	int bs;

	if (pha->abort_flag || pha->protocol_error) return 1;
	bs = pha->proto->get_bus_status(pha);

	if ( (bs & (PPSC_REQ|PPSC_BSY)) != PPSC_BSY) return 1;

	return 0;
}

int ppsc_abort (struct scsi_cmnd * cmd)
{
	PHA *pha = (PHA *)cmd->device->host->hostdata[0];

	printk("%s: Command abort not supported\n",pha->device);
	return FAILED;
}

static void ppsc_reset_pha (PHA *pha)
{
	if (!pha->proto->reset_bus) {
		printk("%s: No reset method available\n",pha->device);
		return;
	}

	ppsc_claim(pha);
	pha->proto->connect(pha);
	pha->proto->reset_bus(pha);
	ssleep(4*HZ);
	pha->proto->disconnect(pha);
	ppsc_unclaim(pha);

	if (!pha->quiet) printk("%s: Bus reset\n",pha->device);
}

int ppsc_reset (struct scsi_cmnd * cmd)
{
	PHA *pha = (PHA *)cmd->device->host->hostdata[0];
	int k = 0;

	if (!pha->proto->reset_bus)
		return FAILED;

	if (pha->cur_cmd) 
		pha->abort_flag = PPSC_DO_RESET;
	
	while (pha->cur_cmd && (k < PPSC_RESET_TMO)) {
		ssleep(HZ/10);
		k += HZ/10;
	}
		
	if (pha->cur_cmd) {
		printk("%s: Driver won't give up for reset\n",pha->device);
		return FAILED;
	}

	ppsc_reset_pha(pha);

	return SUCCESS;
}

#define PROCIN(n,var)						\
	if ((length>n+1)&&(strncmp(buffer,#var"=",n+1)==0)) {	\
		pha->var = simple_strtoul(buffer+n+1,NULL,0);	\
		return length;					\
	}

#define PROCOUT(fmt,val)  len+=sprintf(buffer+len,fmt"\n",val);

int ppsc_proc_info(struct Scsi_Host *p, char *buffer, char **start, off_t offset,
		   int length, int inout)
{
	int len = 0;
	PHA *pha;

	if (!p) return 0;  /* should never happen */
	pha = (PHA *)p->hostdata[0];

	if (inout) {

		PROCIN(4,mode);
		PROCIN(5,delay);
		PROCIN(7,verbose);
		PROCIN(10,abort_flag);
		PROCIN(4,nice);

		return (-EINVAL);
	}

	PROCOUT("ident:		 %s",pha->ident);
	PROCOUT("base port:	 0x%03x",pha->port);
	PROCOUT("mode:		 %d",pha->mode);
	if (pha->proto->mode_names)
		PROCOUT("mode name:	    %s",pha->proto->mode_names[pha->mode]);
	PROCOUT("delay:		 %d",pha->delay);
	PROCOUT("nice:		 %d",pha->nice);
	PROCOUT("verbose:	 %d",pha->verbose);
	PROCOUT("quiet:		 %d",pha->quiet);
	PROCOUT("tot_cmds:	 %d",pha->tot_cmds);
	PROCOUT("tot_bytes:	 %ld",pha->tot_bytes);
	PROCOUT("tot_errs:	 %d",pha->tot_errs);

	if (pha->pardev) {
		PROCOUT("parport device: %s",pha->parname);
		PROCOUT("claimed:	   %d",pha->claimed);
	}
	if (V_DEBUG) {
		PROCOUT("then:	   %ld",pha->then);
		PROCOUT("timeout:	   %ld",pha->timeout);
		PROCOUT("now:		   %ld",jiffies);
		PROCOUT("timer active:   %d",pha->timer_active);
		PROCOUT("wq_active:	   %d",pha->wq_active);
		PROCOUT("abort_flag:	   %d",pha->abort_flag);
		PROCOUT("return_code:	   %08x",pha->return_code);
		PROCOUT("last_phase:	   %o",pha->last_phase);
		PROCOUT("cmd_count:	   %d",pha->cmd_count);
		PROCOUT("data_count:	   %d",pha->data_count);
		PROCOUT("data_dir:	   %d",pha->data_dir);
		PROCOUT("bulk:	   %d",pha->bulk);
		PROCOUT("tlen:	   %d",pha->tlen);
		PROCOUT("overflow:	   %d",pha->overflow);
	}

	if (offset > len) return 0;

	*start = buffer+offset; len -= offset; 
	if (len > length) len = length;
	return len;
}	

int ppsc_biosparam (struct scsi_device * sdev, struct block_device *bdev, sector_t capacity, int ip[])
{
	ip[0] = 0x40;
	ip[1] = 0x20;
	ip[2] = (unsigned int)(capacity +1) >> 11;
	if (ip[2] > 1024) {
		ip[0] = 0xff;
		ip[1] = 0x3f;
		ip[2] = (unsigned int)(capacity +1) / (0xff * 0x3f);
		if (ip[2] > 1023)
			ip[2] = 1023;
	}
	return 0;
}

/*
 * We declare the first two as globals to avoid reaching the 1024 local frame "limit"
 * But they were originally declared inside the ppsc_inquire function
 * Also, we avoid assigning the local array "inq" to cmnd, since we don't
 * know what could happen to the array memory outside the function scope.
 * I didn't want to deal with malloc for this, so I declared ppsc_inquire_command
 * as a global. Dirty hack, I know, but the old code was not safe.
 */
struct scsi_cmnd ppsc_inquire_cmd;
struct scsi_device ppsc_inquire_dev;
char ppsc_inquire_command[6];
static int ppsc_inquire (PHA *pha, int target, char *buf)
{
    char inq[6] = {0x12,0,0,0,36,0};
    
	ppsc_inquire_dev.host = pha->host_ptr;
	ppsc_inquire_dev.id = target;
	ppsc_inquire_cmd.device = &ppsc_inquire_dev;
	ppsc_inquire_cmd.cmd_len = 6;
    #if LINUX_VERSION_CODE > KERNEL_VERSION(3,13,0)
        memcpy(ppsc_inquire_command, inq, 6);
        ppsc_inquire_cmd.cmnd = ppsc_inquire_command;
    #else
        memcpy(ppsc_inquire_cmd.cmnd, inq, 6);
    #endif
	#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,24)
		ppsc_inquire_cmd.sdb.table.nents = 0;
		ppsc_inquire_cmd.sdb.table.sgl = (struct scatterlist *)buf;
		ppsc_inquire_cmd.sdb.length = 36;
	#else
		ppsc_inquire_cmd.use_sg = 0;
		ppsc_inquire_cmd.request_buffer = buf;
		ppsc_inquire_cmd.request_bufflen = 36;
	#endif

	return ppsc_command(&ppsc_inquire_cmd);
	
}

static void ppsc_test_mode (PHA *pha, int mode)
{
	int i, t, s, e, f, g, ok, old_mode;
	char	ibuf[38]; 

	if ((mode >= pha->proto->epp_first) &&
	    !(pha->pardev->port->modes & PARPORT_MODE_EPP))
		return;

	old_mode = pha->mode;
	pha->mode = mode;

	e = -1;	 f = -1;  g = 0;

	if (pha->proto->test_proto) {
		ppsc_claim(pha);
		e = pha->proto->test_proto(pha);
		ppsc_unclaim(pha);
	}

	if (e <= 0) {
		f = 0;
		for (t=0;t<8;t++) {
			s = ppsc_inquire(pha,t,ibuf);
			if (s == DID_NO_CONNECT << 16) continue;
			if (s) { f++; 
			break; 
			} 
			if (V_FULL) {
				for (i=0;i<36;i++) 
					if ((ibuf[i] < ' ') || (ibuf[i] >= '~')) ibuf[i] = '.';
				ibuf[36] = 0;
				printk("%s: port 0x%x mode %d targ %d: %s\n",
				       pha->device,pha->port,mode,t,ibuf);
			}
			g++;
		}
		if (f)  ppsc_reset_pha(pha);
	}

	ok = (e<=0) && (f == 0);

	if (!ok) pha->mode = old_mode;

	if (V_PROBE) printk("%s: port 0x%3x mode %d test %s (%d,%d,%d)\n",
			    pha->device,pha->port,mode,ok?"passed":"failed",e,f,g);
}


int ppsc_release_pha (PHA *pha)
{
	if (pha->proto->release) pha->proto->release(pha);

	ppsc_unregister_parport(pha);

	/* MOD_DEC_USE_COUNT; */

	return 0;
}


int ppsc_detect (PSP *proto, struct scsi_host_template *tpnt, int verbose)
{
	int i, m, p, d, n, s, z;
	struct ppsc_port_list_struct *next_port = NULL; /* shut gcc up */
	int user_specified = 1;
	PHA *pha;
	int host_count = 0;
	struct Scsi_Host *hreg;

	m = 0;
	for (i=0;i<4;i++) if ((*proto->params[i])[PPSC_PARM_PORT] != -1) m++;

	if (!m) {
		/* Just take parports from the list as they come. */
		next_port = ppsc_port_list;
		user_specified = 0;
	}

	tpnt->this_id = PPSC_INITIATOR;

	for (i=0;i<4;i++) {
		if (!user_specified) {
			if (!next_port)
				break;

			p = next_port->port->number;
			next_port = next_port->next;
		}
		else {
			p = (*proto->params[i])[PPSC_PARM_PORT];
			if (p < 0)
				continue;
		}

		m = (*proto->params[i])[PPSC_PARM_MODE];
		n = (*proto->params[i])[PPSC_PARM_NICE];
		if (n == -1) n = PPSC_DEF_NICE;
		d = (*proto->params[i])[PPSC_PARM_DLY];
		if (d == -1) d = proto->default_delay;
		s = (*proto->params[i])[PPSC_PARM_SGTS];
		if (s == -1) s = proto->default_sg_tablesize;
		z = (*proto->params[i])[PPSC_PARM_SLOW];
		if (z == -1) z = 0;	

		/* MOD_INC_USE_COUNT; */

		pha = &(((*proto->hosts)[i]));

		pha->proto = proto;

		pha->port = p;
		pha->delay = d;
		pha->nice = n;

		d = sizeof(pha->device)-3;
		p = strlen(tpnt->name);
		if (p > d) p = d;
		for (n=0;n<p;n++) pha->device[n] = tpnt->name[n];
		pha->device[p] = '.';
		pha->device[p+1] = '0' + i;
		pha->device[p+2] = 0;

		#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
			INIT_WORK(&pha->wq, ppsc_tq_int, pha);
		#else
			INIT_WORK(&pha->wq, ppsc_tq_int);
		#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,00)
		init_timer (&pha->timer);
		pha->timer.data = (unsigned long) pha;
		pha->timer.function = ppsc_timer_int;
#else
		timer_setup(&pha->timer, ppsc_timer_int, 0);
#endif

		init_waitqueue_head (&pha->parq);
		pha->pardev = NULL;
		pha->claimed = 0;
		pha->claim_cont = NULL;
		pha->timer_active = 0;
		pha->wq_active = 0;
		pha->timedout = 0;

		pha->cur_cmd = NULL;
		pha->done = NULL;
		pha->abort_flag = 0;
		pha->protocol_error = 0;
		pha->tot_errs = 0;
		pha->tot_cmds = 0;
		pha->tot_bytes = 0;

		for (n=0;n<8;n++) pha->private[n] = 0;

		pha->slow_targets = z;

		if (ppsc_register_parport(pha,verbose)) {
			/* MOD_DEC_USE_COUNT; */
			continue;
		}

		pha->proto->init(pha);

		pha->verbose = verbose;
		pha->quiet = 1;		  /* no errors until probe over */
		if (V_FULL) pha->quiet = 0;	  /* unless we want them ... */

		pha->tmo = PPSC_PROBE_TMO;

		hreg = scsi_register(tpnt,sizeof(PHA*));
		hreg->dma_channel = -1;
		hreg->n_io_port = 0;
		hreg->unique_id = (unsigned long)pha; /* What should we put in here??? */
		hreg->sg_tablesize = s;
		hreg->hostdata[0]=(unsigned long)pha; /* Will be our pointer */

		pha->host_ptr = hreg;

		pha->mode = -1;

		if (m == -1) for (m=0;m<proto->num_modes;m++)
			ppsc_test_mode(pha,m);
		else ppsc_test_mode(pha,m);

		if (pha->mode != -1) {

			pha->quiet = 0;		  /* enable PPSC_FAIL msgs */
			pha->tmo = PPSC_GEN_TMO;
			host_count++;

			printk("%s: %s at 0x%3x mode %d (%s) dly %d nice %d sg %d\n",
			       pha->device,
			       pha->ident,
			       pha->port,
			       pha->mode,
			       (pha->proto->mode_names)?
			       pha->proto->mode_names[pha->mode]:"",
			       pha->delay,
			       pha->nice,
			       hreg->sg_tablesize);

		} else {

			scsi_unregister(hreg);
			ppsc_release_pha(pha);

		}
	}
	return host_count;	
}

int ppsc_release (struct Scsi_Host *host)
{
	PHA *pha = (PHA *) host->hostdata[0];

	return ppsc_release_pha(pha);
}

int ppsc_initialise (void)
{
	int i;

	for (i=0;i<256;i++) ppsc_bulk_map[i] = 0;

/* commands marked in this map will use pseudo-DMA transfers, while
   the rest will use the slow handshaking.
*/

	ppsc_bulk_map[READ_6] = 1;
	ppsc_bulk_map[READ_10] = 1;
	ppsc_bulk_map[READ_BUFFER] = 1;
	ppsc_bulk_map[WRITE_6] = 1;
	ppsc_bulk_map[WRITE_10] = 1;
	ppsc_bulk_map[WRITE_BUFFER] = 1;

	if (parport_register_driver (&ppsc_driver)) {
		printk (KERN_WARNING "ppscsi: couldn't register driver\n");
		return -EIO;
	}

	printk("ppSCSI %s (%s) installed\n",PPSC_VERSION,PPSC_H_VERSION);
	return 0;
}

#ifdef MODULE

int init_module (void)
{
	return ppsc_initialise();
}

void cleanup_module (void)
{
	struct ppsc_port_list_struct *ports, *next;
	parport_unregister_driver (&ppsc_driver);
	for (ports = ppsc_port_list; ports; ports = next) {
		next = ports->next;
		parport_put_port (ports->port);
		kfree (ports);
	}
}

MODULE_LICENSE("GPL");

#endif

EXPORT_SYMBOL(ppsc_make_map);
EXPORT_SYMBOL(ppsc_queuecommand);
EXPORT_SYMBOL(ppsc_abort);
EXPORT_SYMBOL(ppsc_reset);
EXPORT_SYMBOL(ppsc_proc_info);
EXPORT_SYMBOL(ppsc_biosparam);
EXPORT_SYMBOL(ppsc_detect);
EXPORT_SYMBOL(ppsc_release);

/* end of ppscsi.c */

