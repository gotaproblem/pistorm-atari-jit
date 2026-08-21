#include <cxxrtl/cxxrtl.h>

#if defined(CXXRTL_INCLUDE_CAPI_IMPL) || \
    defined(CXXRTL_INCLUDE_VCD_CAPI_IMPL)
#include <cxxrtl/capi/cxxrtl_capi.cc>
#endif

#if defined(CXXRTL_INCLUDE_VCD_CAPI_IMPL)
#include <cxxrtl/capi/cxxrtl_capi_vcd.cc>
#endif

using namespace cxxrtl_yosys;

namespace cxxrtl_design {

// \top: 1
// \src: ../rtl/pistorm_psp3.v:32.1-471.10
struct p_pistorm__psp3__core : public module {
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:254.15-254.22
	wire<3> p_por__cnt;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:251.25-251.35
	wire<1> p_bgack__prev;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:251.9-251.16
	wire<1> p_br__prev;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:250.30-250.39
	wire<8> p_cnt__bgack;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:250.15-250.21
	wire<8> p_cnt__br;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:249.9-249.18
	wire<1> p_dbg__page2;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:248.15-248.21
	wire<8> p_cnt__rd;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:247.15-247.21
	wire<7> p_cnt__wr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:246.9-246.17
	wire<1> p_dbg__mode;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:245.9-245.17
	wire<1> p_st__fight;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:244.25-244.30
	wire<1> p_st__wd;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:244.9-244.16
	wire<1> p_st__berr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:243.16-243.18
	wire<10> p_wd;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:240.16-240.19
	wire<4> p_eng;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:232.16-232.36
	wire<1> p_ctl__clr__sticky__pulse;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:231.16-231.30
	wire<1> p_csr__rd__pending;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:230.16-230.26
	wire<1> p_go__pending;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:228.28-228.41
	wire<1> p_st__bgack__seen;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:228.9-228.19
	wire<1> p_st__br__seen;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:225.15-225.18
	wire<2> p_arb;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:217.15-217.20
	wire<4> p_e__cnt;
	// \init: 7
	// \src: ../rtl/pistorm_psp3.v:198.33-198.39
	wire<3> p_s__ipl2;
	// \init: 7
	// \src: ../rtl/pistorm_psp3.v:198.15-198.21
	wire<3> p_s__ipl1;
	// \init: 3
	// \src: ../rtl/pistorm_psp3.v:197.32-197.39
	wire<2> p_s__bgack;
	// \init: 3
	// \src: ../rtl/pistorm_psp3.v:197.15-197.19
	wire<2> p_s__br;
	// \init: 3
	// \src: ../rtl/pistorm_psp3.v:196.49-196.54
	wire<2> p_s__vpa;
	// \init: 3
	// \src: ../rtl/pistorm_psp3.v:196.32-196.38
	wire<2> p_s__berr;
	// \init: 3
	// \src: ../rtl/pistorm_psp3.v:196.15-196.22
	wire<2> p_s__dtack;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:173.36-173.41
	wire<1> p_lds__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:173.22-173.27
	wire<1> p_uds__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:173.9-173.13
	wire<1> p_as__q;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:172.23-172.27
	wire<1> p_bg__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:172.9-172.14
	wire<1> p_vma__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:171.50-171.54
	wire<1> p_rw__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:171.36-171.41
	wire<1> p_lds__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:171.22-171.27
	wire<1> p_uds__i;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:171.9-171.13
	wire<1> p_as__i;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:164.63-164.73
	wire<1> p_r__csrd__clr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:164.45-164.54
	wire<1> p_r__ack__clr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:164.26-164.36
	wire<1> p_r__csrw__clr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:164.9-164.17
	wire<1> p_r__go__clr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:163.60-163.66
	wire<2> p_s__csrd;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:163.45-163.50
	wire<2> p_s__ack;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:163.29-163.35
	wire<2> p_s__csrw;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:163.15-163.19
	wire<2> p_s__go;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:133.10-133.19
	wire<1> p_pi__d__oe__r;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:132.10-132.26
	wire<1> p_ltch__d__rd__oe__n__r;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:131.58-131.66
	wire<1> p_csrd__req;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:131.42-131.49
	wire<1> p_ack__req;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:131.25-131.33
	wire<1> p_csrw__req;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:131.10-131.16
	wire<1> p_go__req;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:115.16-115.22
	wire<6> p_csr__wv;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:114.16-114.23
	wire<3> p_attr__fc;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:113.50-113.57
	wire<1> p_attr__a0;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:113.32-113.41
	wire<1> p_attr__byte;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:113.16-113.23
	wire<1> p_attr__rd;
	// \src: ../rtl/pistorm_psp3.v:78.24-78.31
	/*input*/ value<1> p_bgack__n;
	// \src: ../rtl/pistorm_psp3.v:77.24-77.28
	/*output*/ value<1> p_bg__n;
	// \src: ../rtl/pistorm_psp3.v:76.24-76.28
	/*input*/ value<1> p_br__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:75.24-75.34
	/*output*/ wire<1> p_halt__drive;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:74.24-74.35
	/*output*/ wire<1> p_reset__drive;
	// \src: ../rtl/pistorm_psp3.v:73.24-73.34
	/*input*/ value<1> p_reset__n__in;
	// \src: ../rtl/pistorm_psp3.v:72.24-72.29
	/*input*/ value<3> p_ipl__n;
	// \src: ../rtl/pistorm_psp3.v:71.24-71.29
	/*output*/ value<1> p_vma__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:70.24-70.29
	/*output*/ wire<1> p_e__clk;
	// \src: ../rtl/pistorm_psp3.v:69.24-69.29
	/*input*/ value<1> p_vpa__n;
	// \src: ../rtl/pistorm_psp3.v:68.24-68.30
	/*input*/ value<1> p_berr__n;
	// \src: ../rtl/pistorm_psp3.v:67.24-67.31
	/*input*/ value<1> p_dtack__n;
	// \src: ../rtl/pistorm_psp3.v:66.24-66.26
	/*output*/ value<1> p_rw;
	// \src: ../rtl/pistorm_psp3.v:65.24-65.29
	/*output*/ value<1> p_lds__n;
	// \src: ../rtl/pistorm_psp3.v:64.24-64.29
	/*output*/ value<1> p_uds__n;
	// \src: ../rtl/pistorm_psp3.v:63.24-63.28
	/*output*/ value<1> p_as__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:62.24-62.30
	/*output*/ wire<1> p_bus__oe;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:61.24-61.30
	/*output*/ wire<3> p_fc__out;
	// \src: ../rtl/pistorm_psp3.v:58.24-58.38
	/*output*/ value<1> p_ltch__d__rd__oe__n;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:57.24-57.35
	/*output*/ wire<1> p_ltch__d__rd__l;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:56.24-56.35
	/*output*/ wire<1> p_ltch__d__rd__u;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:55.24-55.38
	/*output*/ wire<1> p_ltch__d__wr__oe__n;
	// \src: ../rtl/pistorm_psp3.v:54.24-54.35
	/*output*/ value<1> p_ltch__d__wr__l;
	// \src: ../rtl/pistorm_psp3.v:53.24-53.35
	/*output*/ value<1> p_ltch__d__wr__u;
	// \init: 1
	// \src: ../rtl/pistorm_psp3.v:52.24-52.35
	/*output*/ wire<1> p_ltch__a__oe__n;
	// \src: ../rtl/pistorm_psp3.v:51.24-51.33
	/*output*/ value<1> p_ltch__a__24;
	// \src: ../rtl/pistorm_psp3.v:50.24-50.33
	/*output*/ value<1> p_ltch__a__16;
	// \src: ../rtl/pistorm_psp3.v:49.24-49.32
	/*output*/ value<1> p_ltch__a__8;
	// \src: ../rtl/pistorm_psp3.v:48.24-48.32
	/*output*/ value<1> p_ltch__a__0;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:45.24-45.31
	/*output*/ wire<1> p_pi__ipl2;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:44.24-44.31
	/*output*/ wire<1> p_pi__ipl1;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:43.24-43.31
	/*output*/ wire<1> p_pi__berr;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:42.24-42.28
	/*output*/ wire<1> p_busy;
	// \src: ../rtl/pistorm_psp3.v:41.24-41.31
	/*output*/ value<1> p_pi__d__oe;
	// \init: 0
	// \src: ../rtl/pistorm_psp3.v:40.24-40.32
	/*output*/ wire<16> p_pi__d__out;
	// \src: ../rtl/pistorm_psp3.v:39.24-39.31
	/*input*/ value<16> p_pi__d__in;
	// \src: ../rtl/pistorm_psp3.v:38.24-38.30
	/*input*/ value<2> p_pi__cmd;
	// \src: ../rtl/pistorm_psp3.v:37.24-37.29
	/*input*/ value<1> p_pi__rd;
	value<1> prev_p_pi__rd;
	bool negedge_p_pi__rd() const {
		return prev_p_pi__rd.slice<0>().val() && !p_pi__rd.slice<0>().val();
	}
	// \src: ../rtl/pistorm_psp3.v:36.24-36.29
	/*input*/ value<1> p_pi__wr;
	value<1> prev_p_pi__wr;
	bool negedge_p_pi__wr() const {
		return prev_p_pi__wr.slice<0>().val() && !p_pi__wr.slice<0>().val();
	}
	// \src: ../rtl/pistorm_psp3.v:33.24-33.27
	/*input*/ value<1> p_clk;
	value<1> prev_p_clk;
	bool posedge_p_clk() const {
		return !prev_p_clk.slice<0>().val() && p_clk.slice<0>().val();
	}
	bool negedge_p_clk() const {
		return prev_p_clk.slice<0>().val() && !p_clk.slice<0>().val();
	}
	// \src: ../rtl/pistorm_psp3.v:255.15-255.18
	/*outline*/ value<1> p_por;
	// \src: ../rtl/pistorm_psp3.v:227.10-227.18
	/*outline*/ value<1> p_ext__owns;
	// \src: ../rtl/pistorm_psp3.v:226.10-226.21
	/*outline*/ value<1> p_engine__idle;
	// \src: ../rtl/pistorm_psp3.v:214.10-214.17
	/*outline*/ value<1> p_bgack__a;
	// \src: ../rtl/pistorm_psp3.v:213.10-213.14
	/*outline*/ value<1> p_br__a;
	// \src: ../rtl/pistorm_psp3.v:211.10-211.16
	/*outline*/ value<1> p_berr__a;
	// \src: ../rtl/pistorm_psp3.v:98.10-98.17
	/*outline*/ value<1> p_sel__csr;
	// \src: ../rtl/pistorm_psp3.v:97.10-97.17
	/*outline*/ value<1> p_sel__ahi;
	// \src: ../rtl/pistorm_psp3.v:96.10-96.17
	/*outline*/ value<1> p_sel__alo;
	// \src: ../rtl/pistorm_psp3.v:95.10-95.18
	/*outline*/ value<1> p_sel__data;
	p_pistorm__psp3__core(interior) {}
	p_pistorm__psp3__core() {
		reset();
	};

	void reset() override;

	bool eval(performer *performer = nullptr) override;

	template<class ObserverT>
	bool commit(ObserverT &observer) {
		bool changed = false;
		if (p_por__cnt.commit(observer)) changed = true;
		if (p_bgack__prev.commit(observer)) changed = true;
		if (p_br__prev.commit(observer)) changed = true;
		if (p_cnt__bgack.commit(observer)) changed = true;
		if (p_cnt__br.commit(observer)) changed = true;
		if (p_dbg__page2.commit(observer)) changed = true;
		if (p_cnt__rd.commit(observer)) changed = true;
		if (p_cnt__wr.commit(observer)) changed = true;
		if (p_dbg__mode.commit(observer)) changed = true;
		if (p_st__fight.commit(observer)) changed = true;
		if (p_st__wd.commit(observer)) changed = true;
		if (p_st__berr.commit(observer)) changed = true;
		if (p_wd.commit(observer)) changed = true;
		if (p_eng.commit(observer)) changed = true;
		if (p_ctl__clr__sticky__pulse.commit(observer)) changed = true;
		if (p_csr__rd__pending.commit(observer)) changed = true;
		if (p_go__pending.commit(observer)) changed = true;
		if (p_st__bgack__seen.commit(observer)) changed = true;
		if (p_st__br__seen.commit(observer)) changed = true;
		if (p_arb.commit(observer)) changed = true;
		if (p_e__cnt.commit(observer)) changed = true;
		if (p_s__ipl2.commit(observer)) changed = true;
		if (p_s__ipl1.commit(observer)) changed = true;
		if (p_s__bgack.commit(observer)) changed = true;
		if (p_s__br.commit(observer)) changed = true;
		if (p_s__vpa.commit(observer)) changed = true;
		if (p_s__berr.commit(observer)) changed = true;
		if (p_s__dtack.commit(observer)) changed = true;
		if (p_lds__q.commit(observer)) changed = true;
		if (p_uds__q.commit(observer)) changed = true;
		if (p_as__q.commit(observer)) changed = true;
		if (p_bg__i.commit(observer)) changed = true;
		if (p_vma__i.commit(observer)) changed = true;
		if (p_rw__i.commit(observer)) changed = true;
		if (p_lds__i.commit(observer)) changed = true;
		if (p_uds__i.commit(observer)) changed = true;
		if (p_as__i.commit(observer)) changed = true;
		if (p_r__csrd__clr.commit(observer)) changed = true;
		if (p_r__ack__clr.commit(observer)) changed = true;
		if (p_r__csrw__clr.commit(observer)) changed = true;
		if (p_r__go__clr.commit(observer)) changed = true;
		if (p_s__csrd.commit(observer)) changed = true;
		if (p_s__ack.commit(observer)) changed = true;
		if (p_s__csrw.commit(observer)) changed = true;
		if (p_s__go.commit(observer)) changed = true;
		if (p_pi__d__oe__r.commit(observer)) changed = true;
		if (p_ltch__d__rd__oe__n__r.commit(observer)) changed = true;
		if (p_csrd__req.commit(observer)) changed = true;
		if (p_ack__req.commit(observer)) changed = true;
		if (p_csrw__req.commit(observer)) changed = true;
		if (p_go__req.commit(observer)) changed = true;
		if (p_csr__wv.commit(observer)) changed = true;
		if (p_attr__fc.commit(observer)) changed = true;
		if (p_attr__a0.commit(observer)) changed = true;
		if (p_attr__byte.commit(observer)) changed = true;
		if (p_attr__rd.commit(observer)) changed = true;
		if (p_halt__drive.commit(observer)) changed = true;
		if (p_reset__drive.commit(observer)) changed = true;
		if (p_e__clk.commit(observer)) changed = true;
		if (p_bus__oe.commit(observer)) changed = true;
		if (p_fc__out.commit(observer)) changed = true;
		if (p_ltch__d__rd__l.commit(observer)) changed = true;
		if (p_ltch__d__rd__u.commit(observer)) changed = true;
		if (p_ltch__d__wr__oe__n.commit(observer)) changed = true;
		if (p_ltch__a__oe__n.commit(observer)) changed = true;
		if (p_pi__ipl2.commit(observer)) changed = true;
		if (p_pi__ipl1.commit(observer)) changed = true;
		if (p_pi__berr.commit(observer)) changed = true;
		if (p_busy.commit(observer)) changed = true;
		if (p_pi__d__out.commit(observer)) changed = true;
		prev_p_pi__rd = p_pi__rd;
		prev_p_pi__wr = p_pi__wr;
		prev_p_clk = p_clk;
		return changed;
	}

	bool commit() override {
		observer observer;
		return commit<>(observer);
	}

	void debug_eval();
	debug_outline debug_eval_outline { std::bind(&p_pistorm__psp3__core::debug_eval, this) };

	void debug_info(debug_items *items, debug_scopes *scopes, std::string path, metadata_map &&cell_attrs = {}) override;
}; // struct p_pistorm__psp3__core

void p_pistorm__psp3__core::reset() {
	p_por__cnt = wire<3>{0u};
	p_bgack__prev = wire<1>{0u};
	p_br__prev = wire<1>{0u};
	p_cnt__bgack = wire<8>{0u};
	p_cnt__br = wire<8>{0u};
	p_dbg__page2 = wire<1>{0u};
	p_cnt__rd = wire<8>{0u};
	p_cnt__wr = wire<7>{0u};
	p_dbg__mode = wire<1>{0u};
	p_st__fight = wire<1>{0u};
	p_st__wd = wire<1>{0u};
	p_st__berr = wire<1>{0u};
	p_wd = wire<10>{0u};
	p_eng = wire<4>{0u};
	p_ctl__clr__sticky__pulse = wire<1>{0u};
	p_csr__rd__pending = wire<1>{0u};
	p_go__pending = wire<1>{0u};
	p_st__bgack__seen = wire<1>{0u};
	p_st__br__seen = wire<1>{0u};
	p_arb = wire<2>{0u};
	p_e__cnt = wire<4>{0u};
	p_s__ipl2 = wire<3>{0x7u};
	p_s__ipl1 = wire<3>{0x7u};
	p_s__bgack = wire<2>{0x3u};
	p_s__br = wire<2>{0x3u};
	p_s__vpa = wire<2>{0x3u};
	p_s__berr = wire<2>{0x3u};
	p_s__dtack = wire<2>{0x3u};
	p_lds__q = wire<1>{0x1u};
	p_uds__q = wire<1>{0x1u};
	p_as__q = wire<1>{0x1u};
	p_bg__i = wire<1>{0x1u};
	p_vma__i = wire<1>{0x1u};
	p_rw__i = wire<1>{0x1u};
	p_lds__i = wire<1>{0x1u};
	p_uds__i = wire<1>{0x1u};
	p_as__i = wire<1>{0x1u};
	p_r__csrd__clr = wire<1>{0u};
	p_r__ack__clr = wire<1>{0u};
	p_r__csrw__clr = wire<1>{0u};
	p_r__go__clr = wire<1>{0u};
	p_s__csrd = wire<2>{0u};
	p_s__ack = wire<2>{0u};
	p_s__csrw = wire<2>{0u};
	p_s__go = wire<2>{0u};
	p_pi__d__oe__r = wire<1>{0u};
	p_ltch__d__rd__oe__n__r = wire<1>{0x1u};
	p_csrd__req = wire<1>{0u};
	p_ack__req = wire<1>{0u};
	p_csrw__req = wire<1>{0u};
	p_go__req = wire<1>{0u};
	p_csr__wv = wire<6>{0u};
	p_attr__fc = wire<3>{0u};
	p_attr__a0 = wire<1>{0u};
	p_attr__byte = wire<1>{0u};
	p_attr__rd = wire<1>{0u};
	p_halt__drive = wire<1>{0u};
	p_reset__drive = wire<1>{0u};
	p_e__clk = wire<1>{0u};
	p_bus__oe = wire<1>{0u};
	p_fc__out = wire<3>{0u};
	p_ltch__d__rd__l = wire<1>{0u};
	p_ltch__d__rd__u = wire<1>{0u};
	p_ltch__d__wr__oe__n = wire<1>{0x1u};
	p_ltch__a__oe__n = wire<1>{0x1u};
	p_pi__ipl2 = wire<1>{0u};
	p_pi__ipl1 = wire<1>{0u};
	p_pi__berr = wire<1>{0u};
	p_busy = wire<1>{0u};
	p_pi__d__out = wire<16>{0u};
}

bool p_pistorm__psp3__core::eval(performer *performer) {
	bool converged = true;
	bool negedge_p_pi__rd = this->negedge_p_pi__rd();
	bool negedge_p_pi__wr = this->negedge_p_pi__wr();
	bool posedge_p_clk = this->posedge_p_clk();
	bool negedge_p_clk = this->negedge_p_clk();
	value<1> i_procmux_24_369__Y;
	value<1> i_procmux_24_344__Y;
	value<1> i_procmux_24_579__Y;
	value<1> i_procmux_24_597__Y;
	value<1> i_procmux_24_602__Y;
	value<4> i_procmux_24_300__Y;
	// \src: ../rtl/pistorm_psp3.v:380.21-380.36
	value<1> i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_380_24_76__Y;
	// \src: ../rtl/pistorm_psp3.v:351.26-351.61
	value<1> i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y;
	// \src: ../rtl/pistorm_psp3.v:330.26-330.45
	value<1> i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_330_24_55__Y;
	value<1> i_procmux_24_270__Y;
	value<1> i_procmux_24_260__Y;
	value<1> i_procmux_24_376__Y;
	// \src: ../rtl/pistorm_psp3.v:255.15-255.18
	value<1> p_por;
	// \src: ../rtl/pistorm_psp3.v:226.10-226.21
	value<1> p_engine__idle;
	// \src: ../rtl/pistorm_psp3.v:214.10-214.17
	value<1> p_bgack__a;
	// \src: ../rtl/pistorm_psp3.v:213.10-213.14
	value<1> p_br__a;
	// \src: ../rtl/pistorm_psp3.v:211.10-211.16
	value<1> p_berr__a;
	// \src: ../rtl/pistorm_psp3.v:98.10-98.17
	value<1> p_sel__csr;
	// \src: ../rtl/pistorm_psp3.v:97.10-97.17
	value<1> p_sel__ahi;
	// \src: ../rtl/pistorm_psp3.v:96.10-96.17
	value<1> p_sel__alo;
	// \src: ../rtl/pistorm_psp3.v:95.10-95.18
	value<1> p_sel__data;
	// cells $procmux$369 $procmux$366
	i_procmux_24_369__Y = (p_r__go__clr.curr ? p_go__pending.curr : (p_s__go.curr.slice<1>().val() ? value<1>{0x1u} : p_go__pending.curr));
	// cells $procmux$597 $procmux$594 $procmux$592
	i_procmux_24_597__Y = (p_r__csrw__clr.curr ? p_ltch__d__rd__oe__n__r.curr : (p_s__csrw.curr.slice<1>().val() ? (p_csr__wv.curr.slice<0>().val() ? value<1>{0x1u} : p_ltch__d__rd__oe__n__r.curr) : p_ltch__d__rd__oe__n__r.curr));
	// cells $procmux$300 $procmux$297 $procmux$295
	i_procmux_24_300__Y = (p_r__csrw__clr.curr ? p_eng.curr : (p_s__csrw.curr.slice<1>().val() ? (p_csr__wv.curr.slice<0>().val() ? value<4>{0u} : p_eng.curr) : p_eng.curr));
	// \src: ../rtl/pistorm_psp3.v:241.27-241.40
	// cell $eq$../rtl/pistorm_psp3.v:241$37
	p_engine__idle = logic_not<1>(p_eng.curr);
	// cells $logic_and$../rtl/pistorm_psp3.v:351$64 $logic_and$../rtl/pistorm_psp3.v:351$62 $eq$../rtl/pistorm_psp3.v:351$61
	i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y = logic_and<1>(logic_and<1>(p_go__pending.curr, logic_not<1>(p_arb.curr)), p_s__br.curr.slice<1>().val());
	// cells $procmux$579 $procmux$576 $procmux$574
	i_procmux_24_579__Y = (p_r__csrw__clr.curr ? p_pi__d__oe__r.curr : (p_s__csrw.curr.slice<1>().val() ? (p_csr__wv.curr.slice<0>().val() ? value<1>{0u} : p_pi__d__oe__r.curr) : p_pi__d__oe__r.curr));
	// \src: ../rtl/pistorm_psp3.v:211.20-211.30
	// cell $not$../rtl/pistorm_psp3.v:211$27
	p_berr__a = not_u<1>(p_s__berr.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp3.v:213.20-213.28
	// cell $not$../rtl/pistorm_psp3.v:213$29
	p_br__a = not_u<1>(p_s__br.curr.slice<1>().val());
	// cells $procmux$376 $procmux$373 $procmux$371
	i_procmux_24_376__Y = (p_r__csrw__clr.curr ? i_procmux_24_369__Y : (p_s__csrw.curr.slice<1>().val() ? (p_csr__wv.curr.slice<0>().val() ? value<1>{0u} : i_procmux_24_369__Y) : i_procmux_24_369__Y));
	// \src: ../rtl/pistorm_psp3.v:97.22-97.36
	// cell $eq$../rtl/pistorm_psp3.v:97$3
	p_sel__ahi = eq_uu<1>(p_pi__cmd, value<2>{0x2u});
	// cells $procmux$602 $procmux$599
	i_procmux_24_602__Y = (p_r__ack__clr.curr ? i_procmux_24_597__Y : (p_s__ack.curr.slice<1>().val() ? value<1>{0x1u} : i_procmux_24_597__Y));
	// cells $logic_or$../rtl/pistorm_psp3.v:380$76 $reduce_and$../rtl/pistorm_psp3.v:380$75
	i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_380_24_76__Y = logic_or<1>(p_berr__a, reduce_and<1>(p_wd.curr));
	// \src: ../rtl/pistorm_psp3.v:330.26-330.45
	// cell $logic_and$../rtl/pistorm_psp3.v:330$55
	i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_330_24_55__Y = logic_and<1>(p_br__a, p_engine__idle);
	// cells $procmux$344 $procmux$341 $procmux$339 $eq$../rtl/pistorm_psp3.v:311$48
	i_procmux_24_344__Y = (p_r__csrd__clr.curr ? p_csr__rd__pending.curr : (p_s__csrd.curr.slice<1>().val() ? (logic_not<1>(p_eng.curr) ? value<1>{0x1u} : p_csr__rd__pending.curr) : p_csr__rd__pending.curr));
	// \src: ../rtl/pistorm_psp3.v:322.13-322.33|../rtl/pistorm_psp3.v:322.9-326.12
	// cell $procmux$270
	i_procmux_24_270__Y = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : p_st__berr.curr);
	// \src: ../rtl/pistorm_psp3.v:322.13-322.33|../rtl/pistorm_psp3.v:322.9-326.12
	// cell $procmux$260
	i_procmux_24_260__Y = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : p_st__wd.curr);
	// \src: ../rtl/pistorm_psp3.v:95.22-95.36
	// cell $eq$../rtl/pistorm_psp3.v:95$1
	p_sel__data = logic_not<1>(p_pi__cmd);
	// \src: ../rtl/pistorm_psp3.v:214.20-214.31
	// cell $not$../rtl/pistorm_psp3.v:214$30
	p_bgack__a = not_u<1>(p_s__bgack.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp3.v:98.22-98.36
	// cell $eq$../rtl/pistorm_psp3.v:98$4
	p_sel__csr = eq_uu<1>(p_pi__cmd, value<2>{0x3u});
	// \src: ../rtl/pistorm_psp3.v:96.22-96.36
	// cell $eq$../rtl/pistorm_psp3.v:96$2
	p_sel__alo = eq_uu<1>(p_pi__cmd, value<2>{0x1u});
	// \src: ../rtl/pistorm_psp3.v:255.22-255.37
	// cell $ne$../rtl/pistorm_psp3.v:255$38
	p_por = ne_uu<1>(p_por__cnt.curr, value<3>{0x7u});
	// \src: ../rtl/pistorm_psp3.v:104.26-104.42
	// cell $and$../rtl/pistorm_psp3.v:104$5
	p_ltch__a__0 = and_uu<1>(p_sel__alo, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:105.26-105.42
	// cell $and$../rtl/pistorm_psp3.v:105$6
	p_ltch__a__8 = and_uu<1>(p_sel__alo, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:106.26-106.42
	// cell $and$../rtl/pistorm_psp3.v:106$7
	p_ltch__a__16 = and_uu<1>(p_sel__ahi, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:107.26-107.42
	// cell $and$../rtl/pistorm_psp3.v:107$8
	p_ltch__a__24 = and_uu<1>(p_sel__ahi, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:108.26-108.42
	// cell $and$../rtl/pistorm_psp3.v:108$9
	p_ltch__d__wr__u = and_uu<1>(p_sel__data, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:109.26-109.42
	// cell $and$../rtl/pistorm_psp3.v:109$10
	p_ltch__d__wr__l = and_uu<1>(p_sel__data, p_pi__wr);
	// \src: ../rtl/pistorm_psp3.v:138.29-138.55
	// cell $or$../rtl/pistorm_psp3.v:138$12
	p_ltch__d__rd__oe__n = or_uu<1>(p_ltch__d__rd__oe__n__r.curr, p_ack__req.curr);
	// cells $and$../rtl/pistorm_psp3.v:139$14 $not$../rtl/pistorm_psp3.v:139$13
	p_pi__d__oe = and_uu<1>(p_pi__d__oe__r.curr, not_u<1>(p_ack__req.curr));
	// \src: ../rtl/pistorm_psp3.v:178.20-178.32
	// cell $and$../rtl/pistorm_psp3.v:178$22
	p_as__n = and_uu<1>(p_as__i.curr, p_as__q.curr);
	// \src: ../rtl/pistorm_psp3.v:179.20-179.33
	// cell $and$../rtl/pistorm_psp3.v:179$23
	p_uds__n = and_uu<1>(p_uds__i.curr, p_uds__q.curr);
	// \src: ../rtl/pistorm_psp3.v:180.20-180.33
	// cell $and$../rtl/pistorm_psp3.v:180$24
	p_lds__n = and_uu<1>(p_lds__i.curr, p_lds__q.curr);
	// cells $procdff$782 $procmux$698
	if (negedge_p_pi__wr) {
		p_csrw__req.next = (p_sel__csr ? value<1>{0x1u} : p_csrw__req.curr);
	}
	if (p_r__csrw__clr.curr == value<1> {1u}) {
		p_csrw__req.next = value<1>{0u};
	}
	// cells $procdff$789 $procmux$704
	if (negedge_p_pi__wr) {
		p_attr__fc.next = (p_sel__ahi ? p_pi__d__in.slice<15,13>().val() : p_attr__fc.curr);
	}
	// cells $procdff$779 $procmux$696
	if (negedge_p_pi__rd) {
		p_ack__req.next = (p_sel__data ? value<1>{0x1u} : p_ack__req.curr);
	}
	if (p_r__ack__clr.curr == value<1> {1u}) {
		p_ack__req.next = value<1>{0u};
	}
	// cells $procdff$788 $procmux$706
	if (negedge_p_pi__wr) {
		p_attr__a0.next = (p_sel__alo ? p_pi__d__in.slice<0>().val() : p_attr__a0.curr);
	}
	// cells $procdff$776 $procmux$694
	if (negedge_p_pi__rd) {
		p_csrd__req.next = (p_sel__csr ? value<1>{0x1u} : p_csrd__req.curr);
	}
	if (p_r__csrd__clr.curr == value<1> {1u}) {
		p_csrd__req.next = value<1>{0u};
	}
	// cells $procdff$787 $procmux$708
	if (negedge_p_pi__wr) {
		p_attr__byte.next = (p_sel__ahi ? p_pi__d__in.slice<11>().val() : p_attr__byte.curr);
	}
	// cells $procdff$786 $procmux$710
	if (negedge_p_pi__wr) {
		p_attr__rd.next = (p_sel__ahi ? p_pi__d__in.slice<12>().val() : p_attr__rd.curr);
	}
	// cells $procdff$785 $procmux$700 $logic_or$../rtl/pistorm_psp3.v:143$17 $logic_and$../rtl/pistorm_psp3.v:143$16
	if (negedge_p_pi__wr) {
		p_go__req.next = (logic_or<1>(p_sel__data, logic_and<1>(p_sel__ahi, p_pi__d__in.slice<12>().val())) ? value<1>{0x1u} : p_go__req.curr);
	}
	if (p_r__go__clr.curr == value<1> {1u}) {
		p_go__req.next = value<1>{0u};
	}
	// cells $procdff$790 $procmux$702
	if (negedge_p_pi__wr) {
		p_csr__wv.next = (p_sel__csr ? p_pi__d__in.slice<5,0>().val() : p_csr__wv.curr);
	}
	// cells $procdff$712 $procmux$692 $procmux$693_CMP0 $ternary$../rtl/pistorm_psp3.v:429$89 $ternary$../rtl/pistorm_psp3.v:430$88 $eq$../rtl/pistorm_psp3.v:227$36 $not$../rtl/pistorm_psp3.v:432$85 $not$../rtl/pistorm_psp3.v:432$86 $not$../rtl/pistorm_psp3.v:432$87
	if (posedge_p_clk) {
		p_pi__d__out.next = (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? (p_dbg__page2.curr ? p_cnt__br.curr.concat(p_cnt__bgack.curr).val() : (p_dbg__mode.curr ? p_st__fight.curr.concat(p_cnt__wr.curr).concat(p_cnt__rd.curr).val() : not_u<1>(p_engine__idle).concat(not_u<1>(p_s__berr.curr.slice<1>().val())).concat(not_u<1>(p_s__dtack.curr.slice<1>().val())).concat(eq_uu<1>(p_arb.curr, value<2>{0x2u})).concat(p_st__br__seen.curr).concat(p_st__bgack__seen.curr).concat(p_st__wd.curr).concat(p_st__berr.curr).concat(value<8>{0x31u}).val())) : p_pi__d__out.curr);
	}
	// cells $procdff$713 $procmux$684 $procmux$685_CMP0 $procmux$689_CMP0 $not$../rtl/pistorm_psp3.v:424$84 $procmux$687 $not$../rtl/pistorm_psp3.v:416$83
	if (posedge_p_clk) {
		p_busy.next = (eq_uu<1>(p_eng.curr, value<4>{0x9u}) ? not_u<1>(p_busy.curr) : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? p_busy.curr : not_u<1>(p_busy.curr)) : p_busy.curr));
	}
	// cells $procdff$714 $procmux$679 $procmux$677 $procmux$678_CMP0 $procmux$674 $procmux$671
	if (posedge_p_clk) {
		p_pi__berr.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? value<1>{0x1u} : (p_r__go__clr.curr ? p_pi__berr.curr : (p_s__go.curr.slice<1>().val() ? value<1>{0u} : p_pi__berr.curr))));
	}
	// cells $procdff$715 $not$../rtl/pistorm_psp3.v:261$40
	if (posedge_p_clk) {
		p_pi__ipl1.next = not_u<1>(p_s__ipl1.curr.slice<2>().val());
	}
	// cells $procdff$716 $not$../rtl/pistorm_psp3.v:262$41
	if (posedge_p_clk) {
		p_pi__ipl2.next = not_u<1>(p_s__ipl2.curr.slice<2>().val());
	}
	// cells $procdff$717 $procmux$158 $ternary$../rtl/pistorm_psp3.v:339$60 $eq$../rtl/pistorm_psp3.v:339$59
	if (posedge_p_clk) {
		p_ltch__a__oe__n.next = (p_por ? value<1>{0x1u} : (logic_not<1>(p_arb.curr) ? value<1>{0u} : value<1>{0x1u}));
	}
	// cells $procdff$718 $procmux$669 $procmux$657 $procmux$658_CMP0 $procmux$659_CMP0 $procmux$668_CMP0 $procmux$666 $procmux$663 $procmux$661
	if (posedge_p_clk) {
		p_ltch__d__wr__oe__n.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x1u}) ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_ltch__d__wr__oe__n.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? p_ltch__d__wr__oe__n.curr : value<1>{0u}) : p_ltch__d__wr__oe__n.curr)) : p_ltch__d__wr__oe__n.curr))));
	}
	// cells $procdff$719 $procmux$187 $procmux$188_CMP0
	if (posedge_p_clk) {
		p_ltch__d__rd__u.next = (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<1>{0x1u} : value<1>{0u});
	}
	// cells $procdff$720 $procmux$177 $procmux$178_CMP0
	if (posedge_p_clk) {
		p_ltch__d__rd__l.next = (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<1>{0x1u} : value<1>{0u});
	}
	// cells $procdff$721 $procmux$649 $procmux$650_CMP0 $procmux$647 $procmux$644
	if (posedge_p_clk) {
		p_fc__out.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_fc__out.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? p_attr__fc.curr : p_fc__out.curr)) : p_fc__out.curr);
	}
	// cells $procdff$722 $procmux$160 $eq$../rtl/pistorm_psp3.v:338$58
	if (posedge_p_clk) {
		p_bus__oe.next = (p_por ? value<1>{0u} : logic_not<1>(p_arb.curr));
	}
	// cells $procdff$723 $procmux$629 $procmux$627 $procmux$624
	if (posedge_p_clk) {
		p_reset__drive.next = (p_por ? value<1>{0u} : (p_r__csrw__clr.curr ? p_reset__drive.curr : (p_s__csrw.curr.slice<1>().val() ? p_csr__wv.curr.slice<1>().val() : p_reset__drive.curr)));
	}
	// cells $procdff$724 $procmux$622 $procmux$620 $procmux$617
	if (posedge_p_clk) {
		p_halt__drive.next = (p_por ? value<1>{0u} : (p_r__csrw__clr.curr ? p_halt__drive.curr : (p_s__csrw.curr.slice<1>().val() ? p_csr__wv.curr.slice<2>().val() : p_halt__drive.curr)));
	}
	// cells $procdff$725 $procmux$615 $procmux$613 $procmux$614_CMP0 $procmux$611
	if (posedge_p_clk) {
		p_ltch__d__rd__oe__n__r.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? value<1>{0u} : i_procmux_24_602__Y) : i_procmux_24_602__Y));
	}
	// cells $procdff$726 $procmux$590 $procmux$588 $procmux$589_CMP0 $procmux$584 $procmux$581
	if (posedge_p_clk) {
		p_pi__d__oe__r.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? value<1>{0x1u} : (p_r__ack__clr.curr ? i_procmux_24_579__Y : (p_s__ack.curr.slice<1>().val() ? value<1>{0u} : i_procmux_24_579__Y))));
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$727
	if (posedge_p_clk) {
		p_s__go.next = p_s__go.curr.slice<0>().concat(p_go__req.curr).val();
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$728
	if (posedge_p_clk) {
		p_s__csrw.next = p_s__csrw.curr.slice<0>().concat(p_csrw__req.curr).val();
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$729
	if (posedge_p_clk) {
		p_s__ack.next = p_s__ack.curr.slice<0>().concat(p_ack__req.curr).val();
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$730
	if (posedge_p_clk) {
		p_s__csrd.next = p_s__csrd.curr.slice<0>().concat(p_csrd__req.curr).val();
	}
	// cells $procdff$731 $procmux$572 $procmux$570 $procmux$568 $procmux$565
	if (posedge_p_clk) {
		p_r__go__clr.next = (p_por ? value<1>{0x1u} : (p_r__go__clr.curr ? (p_s__go.curr.slice<1>().val() ? p_r__go__clr.curr : value<1>{0u}) : (p_s__go.curr.slice<1>().val() ? value<1>{0x1u} : p_r__go__clr.curr)));
	}
	// cells $procdff$732 $procmux$563 $procmux$561 $procmux$559 $procmux$556
	if (posedge_p_clk) {
		p_r__csrw__clr.next = (p_por ? value<1>{0x1u} : (p_r__csrw__clr.curr ? (p_s__csrw.curr.slice<1>().val() ? p_r__csrw__clr.curr : value<1>{0u}) : (p_s__csrw.curr.slice<1>().val() ? value<1>{0x1u} : p_r__csrw__clr.curr)));
	}
	// cells $procdff$733 $procmux$554 $procmux$552 $procmux$550 $procmux$547
	if (posedge_p_clk) {
		p_r__ack__clr.next = (p_por ? value<1>{0x1u} : (p_r__ack__clr.curr ? (p_s__ack.curr.slice<1>().val() ? p_r__ack__clr.curr : value<1>{0u}) : (p_s__ack.curr.slice<1>().val() ? value<1>{0x1u} : p_r__ack__clr.curr)));
	}
	// cells $procdff$734 $procmux$545 $procmux$543 $procmux$541 $procmux$538
	if (posedge_p_clk) {
		p_r__csrd__clr.next = (p_por ? value<1>{0x1u} : (p_r__csrd__clr.curr ? (p_s__csrd.curr.slice<1>().val() ? p_r__csrd__clr.curr : value<1>{0u}) : (p_s__csrd.curr.slice<1>().val() ? value<1>{0x1u} : p_r__csrd__clr.curr)));
	}
	// cells $procdff$735 $procmux$536 $procmux$528 $procmux$529_CMP0 $procmux$535_CMP0 $procmux$533 $procmux$530
	if (posedge_p_clk) {
		p_as__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? value<1>{0u} : value<1>{0x1u})) : p_as__i.curr)));
	}
	// cells $procdff$736 $procmux$519 $procmux$507 $procmux$508_CMP0 $procmux$509_CMP0 $procmux$518_CMP0 $ternary$../rtl/pistorm_psp3.v:373$71 $procmux$516 $procmux$513 $procmux$511 $ternary$../rtl/pistorm_psp3.v:359$67
	if (posedge_p_clk) {
		p_uds__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? (p_attr__byte.curr ? p_attr__a0.curr : value<1>{0u}) : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? (p_attr__byte.curr ? p_attr__a0.curr : value<1>{0u}) : value<1>{0x1u}) : value<1>{0x1u})) : p_uds__i.curr))));
	}
	// cells $procdff$737 $procmux$498 $procmux$486 $procmux$487_CMP0 $procmux$488_CMP0 $procmux$497_CMP0 $ternary$../rtl/pistorm_psp3.v:374$73 $not$../rtl/pistorm_psp3.v:374$72 $procmux$495 $procmux$492 $procmux$490 $ternary$../rtl/pistorm_psp3.v:360$69 $not$../rtl/pistorm_psp3.v:360$68
	if (posedge_p_clk) {
		p_lds__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? (p_attr__byte.curr ? not_u<1>(p_attr__a0.curr) : value<1>{0u}) : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? (p_attr__byte.curr ? not_u<1>(p_attr__a0.curr) : value<1>{0u}) : value<1>{0x1u}) : value<1>{0x1u})) : p_lds__i.curr))));
	}
	// cells $procdff$738 $procmux$477 $procmux$469 $procmux$470_CMP0 $procmux$476_CMP0 $procmux$474 $procmux$471
	if (posedge_p_clk) {
		p_rw__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? value<1>{0x1u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0x1u} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? p_attr__rd.curr : value<1>{0x1u})) : p_rw__i.curr)));
	}
	// cells $procdff$739 $procmux$461 $procmux$449 $procmux$450_CMP0 $procmux$459_CMP0 $procmux$460_CMP0 $procmux$457 $procmux$454 $procmux$451
	if (posedge_p_clk) {
		p_vma__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<1>{0x1u} : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? (i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_380_24_76__Y ? p_vma__i.curr : (p_s__dtack.curr.slice<1>().val() ? (p_s__vpa.curr.slice<1>().val() ? p_vma__i.curr : value<1>{0u}) : p_vma__i.curr)) : (logic_not<1>(p_eng.curr) ? value<1>{0x1u} : p_vma__i.curr))));
	}
	// cells $procdff$740 $procmux$440 $procmux$435 $procmux$436_CMP0 $procmux$439_CMP0 $procmux$433 $procmux$430 $procmux$437
	if (posedge_p_clk) {
		p_bg__i.next = (p_por ? value<1>{0x1u} : (eq_uu<1>(p_arb.curr, value<2>{0x1u}) ? (p_s__bgack.curr.slice<1>().val() ? (p_s__br.curr.slice<1>().val() ? value<1>{0x1u} : p_bg__i.curr) : value<1>{0x1u}) : (logic_not<1>(p_arb.curr) ? (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_330_24_55__Y ? value<1>{0u} : p_bg__i.curr) : p_bg__i.curr)));
	}
	// cells $procdff$741 $procmux$426 $procmux$412 $procmux$413_CMP0 $procmux$416_CMP0 $procmux$422_CMP0 $procmux$425_CMP0 $procmux$414 $procmux$420 $procmux$417 $procmux$423
	if (posedge_p_clk) {
		p_arb.next = (p_por ? value<2>{0u} : (eq_uu<1>(p_arb.curr, value<2>{0x3u}) ? value<2>{0u} : (eq_uu<1>(p_arb.curr, value<2>{0x2u}) ? (p_s__bgack.curr.slice<1>().val() ? value<2>{0x3u} : p_arb.curr) : (eq_uu<1>(p_arb.curr, value<2>{0x1u}) ? (p_s__bgack.curr.slice<1>().val() ? (p_s__br.curr.slice<1>().val() ? value<2>{0x3u} : p_arb.curr) : value<2>{0x2u}) : (logic_not<1>(p_arb.curr) ? (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_330_24_55__Y ? value<2>{0x1u} : p_arb.curr) : p_arb.curr)))));
	}
	// cells $procdff$742 $procmux$410 $procmux$408 $procmux$406
	if (posedge_p_clk) {
		p_st__br__seen.next = (p_por ? value<1>{0u} : (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_s__br.curr.slice<1>().val() ? p_st__br__seen.curr : value<1>{0x1u})));
	}
	// cells $procdff$743 $procmux$404 $procmux$402 $procmux$400
	if (posedge_p_clk) {
		p_st__bgack__seen.next = (p_por ? value<1>{0u} : (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_s__bgack.curr.slice<1>().val() ? p_st__bgack__seen.curr : value<1>{0x1u})));
	}
	// cells $procdff$744 $procmux$398 $procmux$396 $procmux$397_CMP0 $procmux$394 $procmux$391
	if (posedge_p_clk) {
		p_go__pending.next = (p_por ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? i_procmux_24_376__Y : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? value<1>{0u} : i_procmux_24_376__Y)) : i_procmux_24_376__Y));
	}
	// cells $procdff$745 $procmux$364 $procmux$362 $procmux$363_CMP0 $procmux$360
	if (posedge_p_clk) {
		p_csr__rd__pending.next = (p_por ? value<1>{0u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<1>{0u} : i_procmux_24_344__Y) : i_procmux_24_344__Y));
	}
	// cells $procdff$746 $procmux$167 $procmux$164 $procmux$162
	if (posedge_p_clk) {
		p_ctl__clr__sticky__pulse.next = (p_r__csrw__clr.curr ? value<1>{0u} : (p_s__csrw.curr.slice<1>().val() ? (p_csr__wv.curr.slice<3>().val() ? value<1>{0x1u} : value<1>{0u}) : value<1>{0u}));
	}
	// cells $procdff$747 $procmux$337 $procmux$303 $procmux$304_CMP0 $procmux$305_CMP0 $procmux$306_CMP0 $procmux$307_CMP0 $procmux$311_CMP0 $procmux$312_CMP0 $procmux$313_CMP0 $procmux$319_CMP0 $procmux$328_CMP0 $procmux$329_CMP0 $procmux$330_CMP0 $procmux$336_CMP0 $procmux$309 $procmux$317 $logic_or$../rtl/pistorm_psp3.v:392$80 $reduce_and$../rtl/pistorm_psp3.v:392$79 $procmux$314 $eq$../rtl/pistorm_psp3.v:394$81 $ternary$../rtl/pistorm_psp3.v:395$82 $procmux$326 $procmux$323 $procmux$320 $ternary$../rtl/pistorm_psp3.v:383$77 $procmux$334 $procmux$331 $ternary$../rtl/pistorm_psp3.v:363$70
	if (posedge_p_clk) {
		p_eng.next = (p_por ? value<4>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? value<4>{0x5u} : (eq_uu<1>(p_eng.curr, value<4>{0xcu}) ? value<4>{0x7u} : (eq_uu<1>(p_eng.curr, value<4>{0x8u}) ? value<4>{0x9u} : (eq_uu<1>(p_eng.curr, value<4>{0x7u}) ? value<4>{0x8u} : (eq_uu<1>(p_eng.curr, value<4>{0x6u}) ? (p_attr__rd.curr ? value<4>{0x7u} : value<4>{0u}) : (eq_uu<1>(p_eng.curr, value<4>{0x5u}) ? value<4>{0x6u} : (eq_uu<1>(p_eng.curr, value<4>{0x4u}) ? value<4>{0x5u} : (eq_uu<1>(p_eng.curr, value<4>{0xau}) ? (logic_or<1>(p_berr__a, reduce_and<1>(p_wd.curr)) ? value<4>{0xbu} : (eq_uu<1>(p_e__cnt.curr, value<4>{0x9u}) ? (p_attr__rd.curr ? value<4>{0x4u} : value<4>{0x5u}) : i_procmux_24_300__Y)) : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? (i_logic__or_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_380_24_76__Y ? value<4>{0xbu} : (p_s__dtack.curr.slice<1>().val() ? (p_s__vpa.curr.slice<1>().val() ? i_procmux_24_300__Y : value<4>{0xau}) : (p_attr__rd.curr ? value<4>{0x4u} : value<4>{0x5u}))) : (eq_uu<1>(p_eng.curr, value<4>{0x2u}) ? value<4>{0x3u} : (eq_uu<1>(p_eng.curr, value<4>{0x1u}) ? value<4>{0x2u} : (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? value<4>{0xcu} : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? value<4>{0x3u} : value<4>{0x2u}) : i_procmux_24_300__Y)) : value<4>{0u})))))))))))));
	}
	// cells $procdff$748 $procmux$293 $procmux$289 $procmux$290_CMP0 $procmux$291_CMP0 $procmux$292_CMP0 $add$../rtl/pistorm_psp3.v:391$78 $add$../rtl/pistorm_psp3.v:379$74
	if (posedge_p_clk) {
		p_wd.next = (p_por ? value<10>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xau}) ? add_uu<10>(p_wd.curr, value<10>{0x1u}) : (eq_uu<1>(p_eng.curr, value<4>{0x3u}) ? add_uu<10>(p_wd.curr, value<10>{0x1u}) : (logic_not<1>(p_eng.curr) ? value<10>{0u} : p_wd.curr))));
	}
	// cells $procdff$749 $procmux$278 $procmux$276 $procmux$277_CMP0 $procmux$274
	if (posedge_p_clk) {
		p_st__berr.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? (p_s__berr.curr.slice<1>().val() ? i_procmux_24_270__Y : value<1>{0x1u}) : i_procmux_24_270__Y));
	}
	// cells $procdff$750 $procmux$268 $procmux$266 $procmux$267_CMP0 $procmux$264
	if (posedge_p_clk) {
		p_st__wd.next = (p_por ? value<1>{0u} : (eq_uu<1>(p_eng.curr, value<4>{0xbu}) ? (p_s__berr.curr.slice<1>().val() ? value<1>{0x1u} : i_procmux_24_260__Y) : i_procmux_24_260__Y));
	}
	// cells $procdff$751 $procmux$258 $procmux$256 $procmux$253 $procmux$251 $logic_or$../rtl/pistorm_psp3.v:276$44 $logic_not$../rtl/pistorm_psp3.v:276$43
	if (posedge_p_clk) {
		p_st__fight.next = (p_ctl__clr__sticky__pulse.curr ? value<1>{0u} : (p_r__go__clr.curr ? p_st__fight.curr : (p_s__go.curr.slice<1>().val() ? (logic_or<1>(logic_not<1>(p_ltch__d__rd__oe__n__r.curr), p_pi__d__oe__r.curr) ? value<1>{0x1u} : p_st__fight.curr) : p_st__fight.curr)));
	}
	// cells $procdff$752 $procmux$249 $procmux$246
	if (posedge_p_clk) {
		p_dbg__mode.next = (p_r__csrw__clr.curr ? p_dbg__mode.curr : (p_s__csrw.curr.slice<1>().val() ? p_csr__wv.curr.slice<4>().val() : p_dbg__mode.curr));
	}
	// cells $procdff$753 $procmux$244 $procmux$245_CMP0 $procmux$242 $procmux$239 $procmux$237 $add$../rtl/pistorm_psp3.v:354$66
	if (posedge_p_clk) {
		p_cnt__wr.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_cnt__wr.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? p_cnt__wr.curr : add_uu<7>(p_cnt__wr.curr, value<7>{0x1u})) : p_cnt__wr.curr)) : p_cnt__wr.curr);
	}
	// cells $procdff$754 $procmux$221 $procmux$222_CMP0 $procmux$219 $procmux$216 $procmux$214 $add$../rtl/pistorm_psp3.v:353$65
	if (posedge_p_clk) {
		p_cnt__rd.next = (logic_not<1>(p_eng.curr) ? (p_csr__rd__pending.curr ? p_cnt__rd.curr : (i_logic__and_24__2e__2e__2f_rtl_2f_pistorm__psp3_2e_v_3a_351_24_64__Y ? (p_attr__rd.curr ? add_uu<8>(p_cnt__rd.curr, value<8>{0x1u}) : p_cnt__rd.curr) : p_cnt__rd.curr)) : p_cnt__rd.curr);
	}
	// cells $procdff$755 $procmux$198 $procmux$195
	if (posedge_p_clk) {
		p_dbg__page2.next = (p_r__csrw__clr.curr ? p_dbg__page2.curr : (p_s__csrw.curr.slice<1>().val() ? p_csr__wv.curr.slice<5>().val() : p_dbg__page2.curr));
	}
	// cells $procdff$756 $procmux$193 $logic_and$../rtl/pistorm_psp3.v:318$50 $logic_not$../rtl/pistorm_psp3.v:318$49 $add$../rtl/pistorm_psp3.v:318$51
	if (posedge_p_clk) {
		p_cnt__br.next = (logic_and<1>(p_br__a, logic_not<1>(p_br__prev.curr)) ? add_uu<8>(p_cnt__br.curr, value<8>{0x1u}) : p_cnt__br.curr);
	}
	// cells $procdff$757 $procmux$191 $logic_and$../rtl/pistorm_psp3.v:319$53 $logic_not$../rtl/pistorm_psp3.v:319$52 $add$../rtl/pistorm_psp3.v:319$54
	if (posedge_p_clk) {
		p_cnt__bgack.next = (logic_and<1>(p_bgack__a, logic_not<1>(p_bgack__prev.curr)) ? add_uu<8>(p_cnt__bgack.curr, value<8>{0x1u}) : p_cnt__bgack.curr);
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$758
	if (posedge_p_clk) {
		p_br__prev.next = p_br__a;
	}
	// \src: ../rtl/pistorm_psp3.v:257.5-469.8
	// cell $procdff$759
	if (posedge_p_clk) {
		p_bgack__prev.next = p_bgack__a;
	}
	// cells $procdff$760 $procmux$189 $add$../rtl/pistorm_psp3.v:451$90
	if (posedge_p_clk) {
		p_por__cnt.next = (p_por ? add_uu<3>(p_por__cnt.curr, value<3>{0x1u}) : p_por__cnt.curr);
	}
	// cells $procdff$761 $ge$../rtl/pistorm_psp3.v:220$35
	if (posedge_p_clk) {
		p_e__clk.next = ge_uu<1>(p_e__cnt.curr, value<4>{0x5u});
	}
	// cells $procdff$762 $ternary$../rtl/pistorm_psp3.v:219$34 $eq$../rtl/pistorm_psp3.v:219$32 $add$../rtl/pistorm_psp3.v:219$33
	if (posedge_p_clk) {
		p_e__cnt.next = (eq_uu<1>(p_e__cnt.curr, value<4>{0x9u}) ? value<4>{0u} : add_uu<4>(p_e__cnt.curr, value<4>{0x1u}));
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$763
	if (posedge_p_clk) {
		p_s__dtack.next = p_s__dtack.curr.slice<0>().concat(p_dtack__n).val();
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$764
	if (posedge_p_clk) {
		p_s__berr.next = p_s__berr.curr.slice<0>().concat(p_berr__n).val();
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$765
	if (posedge_p_clk) {
		p_s__vpa.next = p_s__vpa.curr.slice<0>().concat(p_vpa__n).val();
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$766
	if (posedge_p_clk) {
		p_s__br.next = p_s__br.curr.slice<0>().concat(p_br__n).val();
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$767
	if (posedge_p_clk) {
		p_s__bgack.next = p_s__bgack.curr.slice<0>().concat(p_bgack__n).val();
	}
	// \src: ../rtl/pistorm_psp3.v:174.5-176.8
	// cell $procdff$773
	if (negedge_p_clk) {
		p_lds__q.next = p_lds__i.curr;
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$769
	if (posedge_p_clk) {
		p_s__ipl1.next = p_s__ipl1.curr.slice<1,0>().concat(p_ipl__n.slice<1>()).val();
	}
	// \src: ../rtl/pistorm_psp3.v:199.5-208.8
	// cell $procdff$770
	if (posedge_p_clk) {
		p_s__ipl2.next = p_s__ipl2.curr.slice<1,0>().concat(p_ipl__n.slice<2>()).val();
	}
	// \src: ../rtl/pistorm_psp3.v:174.5-176.8
	// cell $procdff$771
	if (negedge_p_clk) {
		p_as__q.next = p_as__i.curr;
	}
	// \src: ../rtl/pistorm_psp3.v:174.5-176.8
	// cell $procdff$772
	if (negedge_p_clk) {
		p_uds__q.next = p_uds__i.curr;
	}
	// connection
	p_rw = p_rw__i.curr;
	// connection
	p_vma__n = p_vma__i.curr;
	// connection
	p_bg__n = p_bg__i.curr;
	return converged;
}

void p_pistorm__psp3__core::debug_eval() {
	// \src: ../rtl/pistorm_psp3.v:241.27-241.40
	// cell $eq$../rtl/pistorm_psp3.v:241$37
	p_engine__idle = logic_not<1>(p_eng.curr);
	// \src: ../rtl/pistorm_psp3.v:211.20-211.30
	// cell $not$../rtl/pistorm_psp3.v:211$27
	p_berr__a = not_u<1>(p_s__berr.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp3.v:213.20-213.28
	// cell $not$../rtl/pistorm_psp3.v:213$29
	p_br__a = not_u<1>(p_s__br.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp3.v:97.22-97.36
	// cell $eq$../rtl/pistorm_psp3.v:97$3
	p_sel__ahi = eq_uu<1>(p_pi__cmd, value<2>{0x2u});
	// \src: ../rtl/pistorm_psp3.v:227.22-227.34
	// cell $eq$../rtl/pistorm_psp3.v:227$36
	p_ext__owns = eq_uu<1>(p_arb.curr, value<2>{0x2u});
	// \src: ../rtl/pistorm_psp3.v:95.22-95.36
	// cell $eq$../rtl/pistorm_psp3.v:95$1
	p_sel__data = logic_not<1>(p_pi__cmd);
	// \src: ../rtl/pistorm_psp3.v:214.20-214.31
	// cell $not$../rtl/pistorm_psp3.v:214$30
	p_bgack__a = not_u<1>(p_s__bgack.curr.slice<1>().val());
	// \src: ../rtl/pistorm_psp3.v:98.22-98.36
	// cell $eq$../rtl/pistorm_psp3.v:98$4
	p_sel__csr = eq_uu<1>(p_pi__cmd, value<2>{0x3u});
	// \src: ../rtl/pistorm_psp3.v:96.22-96.36
	// cell $eq$../rtl/pistorm_psp3.v:96$2
	p_sel__alo = eq_uu<1>(p_pi__cmd, value<2>{0x1u});
	// \src: ../rtl/pistorm_psp3.v:255.22-255.37
	// cell $ne$../rtl/pistorm_psp3.v:255$38
	p_por = ne_uu<1>(p_por__cnt.curr, value<3>{0x7u});
}

CXXRTL_EXTREMELY_COLD
void p_pistorm__psp3__core::debug_info(debug_items *items, debug_scopes *scopes, std::string path, metadata_map &&cell_attrs) {
	assert(path.empty() || path[path.size() - 1] == ' ');
	if (scopes) {
		scopes->add(path.empty() ? path : path.substr(0, path.size() - 1), "pistorm_psp3_core", metadata_map({
			{ "top", UINT64_C(1) },
			{ "src", "../rtl/pistorm_psp3.v:32.1-471.10" },
		}), std::move(cell_attrs));
	}
	if (items) {
		items->add(path, "por", "src\000s../rtl/pistorm_psp3.v:255.15-255.18\000", debug_eval_outline, p_por);
		items->add(path, "por_cnt", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:254.15-254.22\000", p_por__cnt, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bgack_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:251.25-251.35\000", p_bgack__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "br_prev", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:251.9-251.16\000", p_br__prev, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_bgack", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:250.30-250.39\000", p_cnt__bgack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_br", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:250.15-250.21\000", p_cnt__br, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "dbg_page2", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:249.9-249.18\000", p_dbg__page2, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_rd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:248.15-248.21\000", p_cnt__rd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "cnt_wr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:247.15-247.21\000", p_cnt__wr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "dbg_mode", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:246.9-246.17\000", p_dbg__mode, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_fight", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:245.9-245.17\000", p_st__fight, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_wd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:244.25-244.30\000", p_st__wd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_berr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:244.9-244.16\000", p_st__berr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "wd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:243.16-243.18\000", p_wd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "eng", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:240.16-240.19\000", p_eng, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ctl_clr_sticky_pulse", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:232.16-232.36\000", p_ctl__clr__sticky__pulse, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "csr_rd_pending", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:231.16-231.30\000", p_csr__rd__pending, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "go_pending", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:230.16-230.26\000", p_go__pending, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_bgack_seen", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:228.28-228.41\000", p_st__bgack__seen, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "st_br_seen", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:228.9-228.19\000", p_st__br__seen, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ext_owns", "src\000s../rtl/pistorm_psp3.v:227.10-227.18\000", debug_eval_outline, p_ext__owns);
		items->add(path, "engine_idle", "src\000s../rtl/pistorm_psp3.v:226.10-226.21\000", debug_eval_outline, p_engine__idle);
		items->add(path, "arb", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:225.15-225.18\000", p_arb, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "e_cnt", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:217.15-217.20\000", p_e__cnt, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bgack_a", "src\000s../rtl/pistorm_psp3.v:214.10-214.17\000", debug_eval_outline, p_bgack__a);
		items->add(path, "br_a", "src\000s../rtl/pistorm_psp3.v:213.10-213.14\000", debug_eval_outline, p_br__a);
		items->add(path, "berr_a", "src\000s../rtl/pistorm_psp3.v:211.10-211.16\000", debug_eval_outline, p_berr__a);
		items->add(path, "s_ipl2", "init\000u\000\000\000\000\000\000\000\007src\000s../rtl/pistorm_psp3.v:198.33-198.39\000", p_s__ipl2, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_ipl1", "init\000u\000\000\000\000\000\000\000\007src\000s../rtl/pistorm_psp3.v:198.15-198.21\000", p_s__ipl1, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_bgack", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp3.v:197.32-197.39\000", p_s__bgack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_br", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp3.v:197.15-197.19\000", p_s__br, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_vpa", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp3.v:196.49-196.54\000", p_s__vpa, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_berr", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp3.v:196.32-196.38\000", p_s__berr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_dtack", "init\000u\000\000\000\000\000\000\000\003src\000s../rtl/pistorm_psp3.v:196.15-196.22\000", p_s__dtack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "lds_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:173.36-173.41\000", p_lds__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "uds_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:173.22-173.27\000", p_uds__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "as_q", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:173.9-173.13\000", p_as__q, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "bg_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:172.23-172.27\000", p_bg__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "vma_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:172.9-172.14\000", p_vma__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "rw_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:171.50-171.54\000", p_rw__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "lds_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:171.36-171.41\000", p_lds__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "uds_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:171.22-171.27\000", p_uds__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "as_i", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:171.9-171.13\000", p_as__i, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "r_csrd_clr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:164.63-164.73\000", p_r__csrd__clr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "r_ack_clr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:164.45-164.54\000", p_r__ack__clr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "r_csrw_clr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:164.26-164.36\000", p_r__csrw__clr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "r_go_clr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:164.9-164.17\000", p_r__go__clr, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_csrd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:163.60-163.66\000", p_s__csrd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_ack", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:163.45-163.50\000", p_s__ack, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_csrw", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:163.29-163.35\000", p_s__csrw, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "s_go", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:163.15-163.19\000", p_s__go, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_oe_r", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:133.10-133.19\000", p_pi__d__oe__r, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_oe_n_r", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:132.10-132.26\000", p_ltch__d__rd__oe__n__r, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "csrd_req", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:131.58-131.66\000", p_csrd__req, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "ack_req", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:131.42-131.49\000", p_ack__req, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "csrw_req", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:131.25-131.33\000", p_csrw__req, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "go_req", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:131.10-131.16\000", p_go__req, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "csrd_clr", "src\000s../rtl/pistorm_psp3.v:130.37-130.45\000", debug_alias(), p_r__csrd__clr);
		items->add(path, "ack_clr", "src\000s../rtl/pistorm_psp3.v:130.28-130.35\000", debug_alias(), p_r__ack__clr);
		items->add(path, "csrw_clr", "src\000s../rtl/pistorm_psp3.v:130.18-130.26\000", debug_alias(), p_r__csrw__clr);
		items->add(path, "go_clr", "src\000s../rtl/pistorm_psp3.v:130.10-130.16\000", debug_alias(), p_r__go__clr);
		items->add(path, "csr_wv", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:115.16-115.22\000", p_csr__wv, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_fc", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:114.16-114.23\000", p_attr__fc, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_a0", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:113.50-113.57\000", p_attr__a0, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_byte", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:113.32-113.41\000", p_attr__byte, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "attr_rd", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:113.16-113.23\000", p_attr__rd, 0, debug_item::DRIVEN_SYNC);
		items->add(path, "sel_csr", "src\000s../rtl/pistorm_psp3.v:98.10-98.17\000", debug_eval_outline, p_sel__csr);
		items->add(path, "sel_ahi", "src\000s../rtl/pistorm_psp3.v:97.10-97.17\000", debug_eval_outline, p_sel__ahi);
		items->add(path, "sel_alo", "src\000s../rtl/pistorm_psp3.v:96.10-96.17\000", debug_eval_outline, p_sel__alo);
		items->add(path, "sel_data", "src\000s../rtl/pistorm_psp3.v:95.10-95.18\000", debug_eval_outline, p_sel__data);
		items->add(path, "bgack_n", "src\000s../rtl/pistorm_psp3.v:78.24-78.31\000", p_bgack__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "bg_n", "src\000s../rtl/pistorm_psp3.v:77.24-77.28\000", p_bg__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "br_n", "src\000s../rtl/pistorm_psp3.v:76.24-76.28\000", p_br__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "halt_drive", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:75.24-75.34\000", p_halt__drive, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "reset_drive", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:74.24-74.35\000", p_reset__drive, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "reset_n_in", "src\000s../rtl/pistorm_psp3.v:73.24-73.34\000", p_reset__n__in, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "ipl_n", "src\000s../rtl/pistorm_psp3.v:72.24-72.29\000", p_ipl__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "vma_n", "src\000s../rtl/pistorm_psp3.v:71.24-71.29\000", p_vma__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "e_clk", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:70.24-70.29\000", p_e__clk, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "vpa_n", "src\000s../rtl/pistorm_psp3.v:69.24-69.29\000", p_vpa__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "berr_n", "src\000s../rtl/pistorm_psp3.v:68.24-68.30\000", p_berr__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "dtack_n", "src\000s../rtl/pistorm_psp3.v:67.24-67.31\000", p_dtack__n, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "rw", "src\000s../rtl/pistorm_psp3.v:66.24-66.26\000", p_rw, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "lds_n", "src\000s../rtl/pistorm_psp3.v:65.24-65.29\000", p_lds__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "uds_n", "src\000s../rtl/pistorm_psp3.v:64.24-64.29\000", p_uds__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "as_n", "src\000s../rtl/pistorm_psp3.v:63.24-63.28\000", p_as__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "bus_oe", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:62.24-62.30\000", p_bus__oe, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "fc_out", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:61.24-61.30\000", p_fc__out, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_oe_n", "src\000s../rtl/pistorm_psp3.v:58.24-58.38\000", p_ltch__d__rd__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_d_rd_l", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:57.24-57.35\000", p_ltch__d__rd__l, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_rd_u", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:56.24-56.35\000", p_ltch__d__rd__u, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_wr_oe_n", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:55.24-55.38\000", p_ltch__d__wr__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_d_wr_l", "src\000s../rtl/pistorm_psp3.v:54.24-54.35\000", p_ltch__d__wr__l, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_d_wr_u", "src\000s../rtl/pistorm_psp3.v:53.24-53.35\000", p_ltch__d__wr__u, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_a_oe_n", "init\000u\000\000\000\000\000\000\000\001src\000s../rtl/pistorm_psp3.v:52.24-52.35\000", p_ltch__a__oe__n, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "ltch_a_24", "src\000s../rtl/pistorm_psp3.v:51.24-51.33\000", p_ltch__a__24, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_a_16", "src\000s../rtl/pistorm_psp3.v:50.24-50.33\000", p_ltch__a__16, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_a_8", "src\000s../rtl/pistorm_psp3.v:49.24-49.32\000", p_ltch__a__8, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "ltch_a_0", "src\000s../rtl/pistorm_psp3.v:48.24-48.32\000", p_ltch__a__0, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "pi_ipl2", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:45.24-45.31\000", p_pi__ipl2, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_ipl1", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:44.24-44.31\000", p_pi__ipl1, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_berr", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:43.24-43.31\000", p_pi__berr, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "busy", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:42.24-42.28\000", p_busy, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_oe", "src\000s../rtl/pistorm_psp3.v:41.24-41.31\000", p_pi__d__oe, 0, debug_item::OUTPUT|debug_item::DRIVEN_COMB);
		items->add(path, "pi_d_out", "init\000u\000\000\000\000\000\000\000\000src\000s../rtl/pistorm_psp3.v:40.24-40.32\000", p_pi__d__out, 0, debug_item::OUTPUT|debug_item::DRIVEN_SYNC);
		items->add(path, "pi_d_in", "src\000s../rtl/pistorm_psp3.v:39.24-39.31\000", p_pi__d__in, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_cmd", "src\000s../rtl/pistorm_psp3.v:38.24-38.30\000", p_pi__cmd, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_rd", "src\000s../rtl/pistorm_psp3.v:37.24-37.29\000", p_pi__rd, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "pi_wr", "src\000s../rtl/pistorm_psp3.v:36.24-36.29\000", p_pi__wr, 0, debug_item::INPUT|debug_item::UNDRIVEN);
		items->add(path, "clk", "src\000s../rtl/pistorm_psp3.v:33.24-33.27\000", p_clk, 0, debug_item::INPUT|debug_item::UNDRIVEN);
	}
}

} // namespace cxxrtl_design

extern "C"
cxxrtl_toplevel cxxrtl_design_create() {
	return new _cxxrtl_toplevel { std::unique_ptr<cxxrtl_design::p_pistorm__psp3__core>(new cxxrtl_design::p_pistorm__psp3__core) };
}
