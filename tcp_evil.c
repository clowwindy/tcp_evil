/*
 * TCP EVIL
 *
 * TCP-EVIL Congestion control algorithm, based on very few
 * modifications to tcp_hybla
 *
 * This is a demostration of how easily we can increase TCP
 * sending speed from the server side if you ignore fairness
 * and friendliness.
 *
 * DO NOT USE THIS MODULE!
 * REPEAT: DO NOT USE THIS MODULE!
 *
 */

#include <linux/module.h>
#include <net/tcp.h>
#include <asm/i387.h>

/* Tcp evil structure. */
struct evil {
	u8    evil_en;
	u32   snd_cwnd_cents; /* Keeps increment values when it is <1, <<7 */
	u32   rho;	      /* Rho parameter, integer part  */
	u32   rho2;	      /* Rho * Rho, integer part */
	u32   rho_3ls;	      /* Rho parameter, <<3 */
	u32   rho2_7ls;	      /* Rho^2, <<7	*/
	u32   minrtt;	      /* Minimum smoothed round trip time value seen */
};

/* evil reference round trip time (default= 1/200 sec = 5 ms),
   expressed in jiffies */
static int rtt0 = 5;
module_param(rtt0, int, 0644);
MODULE_PARM_DESC(rtt0, "reference rout trip time (ms)");


/* This is called to refresh values for evil parameters */
static inline void evil_recalc_param (struct sock *sk)
{
	struct evil *ca = inet_csk_ca(sk);

	ca->rho_3ls = max_t(u32, tcp_sk(sk)->srtt / msecs_to_jiffies(rtt0), 8);
	ca->rho = ca->rho_3ls >> 3;
	ca->rho2_7ls = (ca->rho_3ls * ca->rho_3ls) << 1;
	ca->rho2 = ca->rho2_7ls >>7;
}

static void evil_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct evil *ca = inet_csk_ca(sk);

	ca->rho = 0;
	ca->rho2 = 0;
	ca->rho_3ls = 0;
	ca->rho2_7ls = 0;
	ca->snd_cwnd_cents = 0;
	ca->evil_en = 1;
	tp->snd_cwnd = 2;
	tp->snd_cwnd_clamp = 65535;

	/* 1st Rho measurement based on initial srtt */
	evil_recalc_param(sk);

	/* set minimum rtt as this is the 1st ever seen */
	ca->minrtt = tp->srtt;
	tp->snd_cwnd = ca->rho;
}

static void evil_state(struct sock *sk, u8 ca_state)
{
	struct evil *ca = inet_csk_ca(sk);
	ca->evil_en = (ca_state == TCP_CA_Open);
}

static inline u32 evil_fraction(u32 odds)
{
	static const u32 fractions[] = {
		128, 139, 152, 165, 181, 197, 215, 234,
	};

	return (odds < ARRAY_SIZE(fractions)) ? fractions[odds] : 128;
}

/* TCP evil main routine.
 * This is the algorithm behavior:
 *     o Recalc evil parameters if min_rtt has changed
 *     o Give cwnd a new value based on the model proposed
 *     o remember increments <1
 */
static void evil_cong_avoid(struct sock *sk, u32 ack, u32 in_flight)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct evil *ca = inet_csk_ca(sk);
	u32 increment, odd, rho_fractions;
	int is_slowstart = 0;

	/*  Recalculate rho only if this srtt is the lowest */
	if (tp->srtt < ca->minrtt){
		evil_recalc_param(sk);
		ca->minrtt = tp->srtt;
	}

	if (!tcp_is_cwnd_limited(sk, in_flight))
		return;

	if (!ca->evil_en) {
		tcp_reno_cong_avoid(sk, ack, in_flight);
		return;
	}

	if (ca->rho == 0)
		evil_recalc_param(sk);

	rho_fractions = ca->rho_3ls - (ca->rho << 3);

	if (tp->snd_cwnd < tp->snd_ssthresh) {
		/*
		 * slow start
		 *      INC = 2^RHO - 1
		 * This is done by splitting the rho parameter
		 * into 2 parts: an integer part and a fraction part.
		 * Inrement<<7 is estimated by doing:
		 *	       [2^(int+fract)]<<7
		 * that is equal to:
		 *	       (2^int)	*  [(2^fract) <<7]
		 * 2^int is straightly computed as 1<<int,
		 * while we will use evil_slowstart_fraction_increment() to
		 * calculate 2^fract in a <<7 value.
		 */
		is_slowstart = 1;
		increment = ((1 << min(ca->rho, 16U)) *
			evil_fraction(rho_fractions)) - 128;
	} else {
		/*
		 * congestion avoidance
		 * INC = RHO^2 / W
		 * as long as increment is estimated as (rho<<7)/window
		 * it already is <<7 and we can easily count its fractions.
		 */
		increment = ca->rho2_7ls / tp->snd_cwnd;
		if (increment < 128)
			tp->snd_cwnd_cnt++;
	}

	odd = increment % 128;
	tp->snd_cwnd += increment >> 7;
	ca->snd_cwnd_cents += odd;

	/* check when fractions goes >=128 and increase cwnd by 1. */
	while (ca->snd_cwnd_cents >= 128) {
		tp->snd_cwnd++;
		ca->snd_cwnd_cents -= 128;
		tp->snd_cwnd_cnt = 0;
	}
	/* check when cwnd has not been incremented for a while */
	if (increment == 0 && odd == 0 && tp->snd_cwnd_cnt >= tp->snd_cwnd) {
		tp->snd_cwnd++;
		tp->snd_cwnd_cnt = 0;
	}
	/* clamp down slowstart cwnd to ssthresh value. */
	if (is_slowstart)
		tp->snd_cwnd = min(tp->snd_cwnd, tp->snd_ssthresh);

	tp->snd_cwnd = min_t(u32, tp->snd_cwnd, tp->snd_cwnd_clamp);
}

/* Slow start threshold is 90% of the congestion window (min 2) */
u32 evil_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 ret = tp->snd_cwnd;
	ret = ret - (ret >> 1U) + (ret >> 2U) + (ret >> 3U) + (ret >> 5U);
	ret = max_t(u32, ret, 2U);
	return ret;
}
EXPORT_SYMBOL_GPL(evil_ssthresh);

static struct tcp_congestion_ops tcp_evil __read_mostly = {
	.init		= evil_init,
	.ssthresh	= evil_ssthresh,
	.min_cwnd	= tcp_reno_min_cwnd,
	.cong_avoid	= evil_cong_avoid,
	.set_state	= evil_state,

	.owner		= THIS_MODULE,
	.name		= "evil"
};

static int __init evil_register(void)
{
	BUILD_BUG_ON(sizeof(struct evil) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_evil);
}

static void __exit evil_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_evil);
}

module_init(evil_register);
module_exit(evil_unregister);

MODULE_AUTHOR("clowwindy");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Evil");
