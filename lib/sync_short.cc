/*
 * Copyright (C) 2013, 2016 Bastian Bloessl <bloessl@ccs-labs.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <ieee802-11/sync_short.h>
#include <gnuradio/io_signature.h>
#include "utils.h"

#include <iostream>

using namespace gr::ieee802_11;

static const int MIN_GAP = 480;
static const int MAX_SAMPLES = 540 * 80;

/*
    sync_short_impl:作用 => 根据输入的数据,检测出数据帧(依据相关结果),再继续传递进行后续处理
    继承自:sync_short,sync_short继承自gunradio的bolck类 => block类中包含一些虚函数(如general_work),需要实现
*/
class sync_short_impl : public sync_short {

public:
sync_short_impl(double threshold, unsigned int min_plateau, bool log, bool debug) :
		block("sync_short",
			gr::io_signature::make3(3, 3, sizeof(gr_complex), sizeof(gr_complex), sizeof(float)),
			gr::io_signature::make(1, 1, sizeof(gr_complex))),
		d_log(log),
		d_debug(debug),
		d_state(SEARCH),     /*d_state 初始化为SEARCH*/
		d_plateau(0),
		d_freq_offset(0),
		d_copied(0),
		MIN_PLATEAU(min_plateau),
		d_threshold(threshold) {

	set_tag_propagation_policy(block::TPP_DONT);
}

/*
    input_items:指针数组,这里共三个指针,前两个指向复数,最后一个指向浮点数
    输入:    in  => usrp采样的数据
            abs => ?
            cor => 采样数据在窗口内的自相关数据
        => If it detects a plateau in the autocorrelation stream, it pipes a fixed number
            of samples into the rest of the signal processing pipeline; otherwise it drops the samples.

*/
int general_work (int noutput_items, gr_vector_int& ninput_items,
		gr_vector_const_void_star& input_items,
		gr_vector_void_star& output_items) {
    
    /*gr_complex       => typedef std::complex<float>			gr_complex;
      gr_vector_int    => typedef std::vector<int>			gr_vector_int;
      gr_vector_const_void_star => typedef std::vector<const void *>		gr_vector_const_void_star; 
    */
	const gr_complex *in = (const gr_complex*)input_items[0];
	const gr_complex *in_abs = (const gr_complex*)input_items[1];
	const float *in_cor = (const float*)input_items[2];

	gr_complex *out = (gr_complex*)output_items[0];

	int noutput = noutput_items;
	int ninput = std::min(std::min(ninput_items[0], ninput_items[1]), ninput_items[2]);

	// dout << "SHORT noutput : " << noutput << " ninput: " << ninput_items[0] << std::endl;

	switch(d_state) {

	case SEARCH: {
		int i;

		for(i = 0; i < ninput; i++) {
			if(in_cor[i] > d_threshold) {
				if(d_plateau < MIN_PLATEAU) {
					d_plateau++;

				} else {
					d_state = COPY;
					d_copied = 0;
					d_freq_offset = arg(in_abs[i]) / 16;
					d_plateau = 0;
					insert_tag(nitems_written(0), d_freq_offset, nitems_read(0) + i);
					dout << "SHORT Frame!" << std::endl;
					break;
				}
			} else {
				d_plateau = 0;
			}
		}

		consume_each(i);
		return 0;
	}

	case COPY: {

		int o = 0;
		while( o < ninput && o < noutput && d_copied < MAX_SAMPLES) {
			if(in_cor[o] > d_threshold) {
				if(d_plateau < MIN_PLATEAU) {
					d_plateau++;

				// there's another frame
				} else if(d_copied > MIN_GAP) {
					d_copied = 0;
					d_plateau = 0;
					d_freq_offset = arg(in_abs[o]) / 16;
					insert_tag(nitems_written(0) + o, d_freq_offset, nitems_read(0) + o);
					dout << "SHORT Frame!" << std::endl;
					break;
				}

			} else {
				d_plateau = 0;
			}

			out[o] = in[o] * exp(gr_complex(0, -d_freq_offset * d_copied));
			o++;
			d_copied++;
		}

		if(d_copied == MAX_SAMPLES) {
			d_state = SEARCH;
		}

		dout << "SHORT copied " << o << std::endl;

		consume_each(o);
		return o;
	}
	}

	throw std::runtime_error("sync short: unknown state");
	return 0;
}

void insert_tag(uint64_t item, double freq_offset, uint64_t input_item) {
    /*若开启了打印日志选项,则打印这句*/
	mylog(boost::format("frame start at in: %2% out: %1%") % item % input_item);

	const pmt::pmt_t key = pmt::string_to_symbol("wifi_start");
	const pmt::pmt_t value = pmt::from_double(freq_offset);
	const pmt::pmt_t srcid = pmt::string_to_symbol(name());
	add_item_tag(0, item, key, value, srcid);
}

private:
	enum {SEARCH, COPY} d_state;
	int d_copied;
	int d_plateau;
	float d_freq_offset;
	const double d_threshold;
	const bool d_log;
	const bool d_debug;
	const unsigned int MIN_PLATEAU;
};

sync_short::sptr
sync_short::make(double threshold, unsigned int min_plateau, bool log, bool debug) {
	return gnuradio::get_initial_sptr(new sync_short_impl(threshold, min_plateau, log, debug));
}
