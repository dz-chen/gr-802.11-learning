/*
 * Copyright (C) 2016 Bastian Bloessl <bloessl@ccs-labs.org>
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

#include "frame_equalizer_impl.h"
#include "equalizer/base.h"
#include "equalizer/comb.h"
#include "equalizer/lms.h"
#include "equalizer/ls.h"
#include "equalizer/sta.h"
#include "utils.h"
#include <gnuradio/io_signature.h>

namespace gr {
namespace ieee802_11 {

frame_equalizer::sptr
frame_equalizer::make(Equalizer algo, double freq, double bw, bool log, bool debug) {
	return gnuradio::get_initial_sptr
		(new frame_equalizer_impl(algo, freq, bw, log, debug));
}


/*
 * 输入port:make(1, 1, 64 * sizeof(gr_complex)) 
 *      取决于输入fft size, fftsize 为64
 *      为什么是64? 因为wifi一个 OFDM 符号是64个子载波(11个保护带宽,48个数据子载波,4个导频,1个DC)
 *
 * 输出port:make(1, 1, 48)) => 输出类型uint8_t(unsigned char); 
 *      为什么是48个输出? => 48个数据子载波(去掉了64个子载波中的保护带宽、导频、DC)
 */
frame_equalizer_impl::frame_equalizer_impl(Equalizer algo, double freq, double bw, bool log, bool debug) :
	gr::block("frame_equalizer",
			gr::io_signature::make(1, 1, 64 * sizeof(gr_complex)),
			gr::io_signature::make(1, 1, 48)),
	d_current_symbol(0), d_log(log), d_debug(debug), d_equalizer(NULL),
	d_freq(freq), d_bw(bw), d_frame_bytes(0), d_frame_symbols(0),
	d_freq_offset_from_synclong(0.0) {
    
    /*
     * 消息传递（异步的，方便其他 block 做出响应）
     * 参考：https://wiki.gnuradio.org/index.php/Message_Passing
     * 注意 message 与 tag 的区别... 
     */
	message_port_register_out(pmt::mp("symbols"));
	d_bpsk = constellation_bpsk::make();
	d_qpsk = constellation_qpsk::make();
	d_16qam = constellation_16qam::make();
	d_64qam = constellation_64qam::make();

	d_frame_mod = d_bpsk;           // 默认是 bpsk

	set_tag_propagation_policy(block::TPP_DONT);
	set_algorithm(algo);
}

frame_equalizer_impl::~frame_equalizer_impl() {
}


/*
 * 设置信道估计算法
 */
void frame_equalizer_impl::set_algorithm(Equalizer algo) {
	gr::thread::scoped_lock lock(d_mutex);
	delete d_equalizer;

	switch(algo) {

	case COMB:
		dout << "Comb" << std::endl;
		d_equalizer = new equalizer::comb();
		break;
	case LS:
		dout << "LS" << std::endl;
		d_equalizer = new equalizer::ls();
		break;
	case LMS:
		dout << "LMS" << std::endl;
		d_equalizer = new equalizer::lms();
		break;
	case STA:
		dout << "STA" << std::endl;
		d_equalizer = new equalizer::sta();
		break;
	default:
		throw std::runtime_error("Algorithm not implemented");
	}
}

void
frame_equalizer_impl::set_bandwidth(double bw) {
	gr::thread::scoped_lock lock(d_mutex);
	d_bw = bw;
}

void
frame_equalizer_impl::set_frequency(double freq) {
	gr::thread::scoped_lock lock(d_mutex);
	d_freq = freq;
}

void
frame_equalizer_impl::forecast (int noutput_items, gr_vector_int &ninput_items_required) {
	ninput_items_required[0] = noutput_items;
}


/*
 * 功能描述：
 *  主要负责 1.相位偏移纠正； 2.信道估计； 3.去掉DC、保护间隔以及导频子载波； 4.解调signal信息
 * 
 * 输入：
 *  长度为64的复数（64对应64个子载波）
 * 输出：
 *  长度为48的uint8_t（48对应48个数据子载波）
 */
int frame_equalizer_impl::general_work (int noutput_items,
		gr_vector_int &ninput_items,
		gr_vector_const_void_star &input_items,
		gr_vector_void_star &output_items) {

	gr::thread::scoped_lock lock(d_mutex);

	const gr_complex *in = (const gr_complex *) input_items[0];
	uint8_t *out = (uint8_t *) output_items[0];   // typedef unsigned char uint8_t

	int i = 0;                                    // 记录消耗的输入item个数
	int o = 0;                                    // 记录输出的item个数

    /*
     * d_current_symbol: 记录当前item的编号,一个symbol就是一个item; 满一帧后重新从0计算
     *                   前两个symbol是LTF(长训练序列)、第三个symbol是signal field
     * d_frame_symbols :  ?
     */
	gr_complex symbols[48];             //  equalize、message_port_pub 能使用上；用于存储信道均衡后的数据子载波采样
	gr_complex current_symbol[64];      // 循环中的当前输入符号,64个子载波;

	dout << "FRAME EQUALIZER: input " << ninput_items[0] << "  output " << noutput_items << std::endl;

	while((i < ninput_items[0]) && (o < noutput_items)) {

		get_tags_in_window(tags, 0, i, i + 1, pmt::string_to_symbol("wifi_start"));

		// new frame
		if(tags.size()) {
			d_current_symbol = 0;
			d_frame_symbols = 0;
			d_frame_mod = d_bpsk;

			d_freq_offset_from_synclong = pmt::to_double(tags.front().value) * d_bw / (2 * M_PI);
			d_epsilon0 = pmt::to_double(tags.front().value) * d_bw / (2 * M_PI * d_freq);
			d_er = 0;

			dout << "epsilon: " << d_epsilon0 << std::endl;
		}

		// not interesting -> skip => 注意：不会输出到out中!!!
		if(d_current_symbol > (d_frame_symbols + 2)) {
			i++;
			continue;
		}

		std::memcpy(current_symbol, in + i*64, 64*sizeof(gr_complex));

		/*
         * compensate sampling offset => 补偿采样偏移
         * TODO：如何解释下面的细节？
         */  
		for(int i = 0; i < 64; i++) {
			current_symbol[i] *= exp(gr_complex(0, 2*M_PI*d_current_symbol*80*(d_epsilon0 + d_er)*(i-32)/64));
		}

		gr_complex p = equalizer::base::POLARITY[(d_current_symbol - 2) % 127];

		double beta;
        /* 
         * 前两个symbol是LTF(即长训练序列)、第三个symbol是signal field
         * std::arg => returns the phase angle of z in the interval (−π; π)
         * 11 25 39 53应该是导频子载波
         */ 
		if(d_current_symbol < 2) {
			beta = arg(
					current_symbol[11] -
					current_symbol[25] +
					current_symbol[39] +
					current_symbol[53]);

		} else {
			beta = arg(
					(current_symbol[11] *  p) +
					(current_symbol[39] *  p) +
					(current_symbol[25] *  p) +
					(current_symbol[53] * -p));
		}

		double er = arg(
				(conj(d_prev_pilots[0]) * current_symbol[11] *  p) +
				(conj(d_prev_pilots[1]) * current_symbol[25] *  p) +
				(conj(d_prev_pilots[2]) * current_symbol[39] *  p) +
				(conj(d_prev_pilots[3]) * current_symbol[53] * -p));

		er *= d_bw / (2 * M_PI * d_freq * 80);

		if(d_current_symbol < 2) {
			d_prev_pilots[0] = current_symbol[11];
			d_prev_pilots[1] = -current_symbol[25];
			d_prev_pilots[2] = current_symbol[39];
			d_prev_pilots[3] = current_symbol[53];
		} else {
			d_prev_pilots[0] = current_symbol[11] *  p;
			d_prev_pilots[1] = current_symbol[25] *  p;
			d_prev_pilots[2] = current_symbol[39] *  p;
			d_prev_pilots[3] = current_symbol[53] * -p;
		}

		// compensate residual frequency offset
		for(int i = 0; i < 64; i++) {
			current_symbol[i] *= exp(gr_complex(0, -beta));
		}

		// update estimate of residual frequency offset
		if(d_current_symbol >= 2) {
			double alpha = 0.1;
			d_er = (1-alpha) * d_er + alpha * er;
		}

        /*
         * do equalization => 进行信道估计/均衡 => 见equalizer/ls.cc
         * 48个数据子载波直接输出到out
         * out + o * 48： 
         */ 
		d_equalizer->equalize(current_symbol, d_current_symbol,symbols, out + o * 48, d_frame_mod);

		/*
         * signal field
         * d_current_symbol == 2 说明是signal field，于是解析signal field，得到整个帧的一些元信息
         * signal 由哪些字段组成:https://sampson.blog.csdn.net/article/details/105232288 
         *  => Rate、 Res Length、 Parity、 Signal tail 
         */ 
		if(d_current_symbol == 2) {
            /*
             * 解码 signal 
             */
			if(decode_signal_field(out + o * 48)) {
				pmt::pmt_t dict = pmt::make_dict();
				dict = pmt::dict_add(dict, pmt::mp("frame_bytes"), pmt::from_uint64(d_frame_bytes));
				dict = pmt::dict_add(dict, pmt::mp("encoding"), pmt::from_uint64(d_frame_encoding));
				dict = pmt::dict_add(dict, pmt::mp("snr"), pmt::from_double(d_equalizer->get_snr()));
				dict = pmt::dict_add(dict, pmt::mp("freq"), pmt::from_double(d_freq));
				dict = pmt::dict_add(dict, pmt::mp("freq_offset"), pmt::from_double(d_freq_offset_from_synclong));
				add_item_tag(0, nitems_written(0) + o,
						pmt::string_to_symbol("wifi_start"),
						dict,
						pmt::string_to_symbol(alias()));
			}
		}

        /*
         * d_current_symbol > 2 开始是真正的数据部分;
         * 此处通过 message 机制，将48个数据子载波对应的复数（已经过信道估计/均衡）放入消息队列，方便其他block处理；
         * 这种情况下，在图形界面中只有一个灰色的 symbols标志，但是没有连线的输出，注意与 out 的区别 ！
         */
		if(d_current_symbol > 2) {
			o++;
			pmt::pmt_t pdu = pmt::make_dict();
			message_port_pub(pmt::mp("symbols"), pmt::cons(pmt::make_dict(), pmt::init_c32vector(48, symbols)));
		}

		i++;
		d_current_symbol++;
	}

	consume(0, i);
	return o;
}


/*
 * 解码 signal field，这部分应该是默认使用的 bpsk 调制；
 * 而数据部分使用的可能是其他调制方式，解码 signal 后才能获得相关信息；
 * rx_bits 是 signal 部分的数据子载波复数经过星座图反算后的比特信息（这里长度应该为48）
 * TODO:deinterleave、viterbi_decoder的作用 ？
 */ 
bool frame_equalizer_impl::decode_signal_field(uint8_t *rx_bits) {

	static ofdm_param ofdm(BPSK_1_2);
	static frame_param frame(ofdm, 0);  // 第二个参数是： psdu_length

    /*
     * 即:d_deinterleaved[i] = rx_bits[interleaver_pattern[i]];
     * Dependent on the Modulation and Coding Scheme (MCS), the bits of a symbol are permuted. 
     * The permutation is the same for all symbols of a frame
     * TODO:了解 MCS
     */
	deinterleave(rx_bits);

    /*
     * Convolutional Decoding and Puncturing
     * 
     * d_deinterleaved[] 长度为48
     * TODO：viterbi_decoder细节
     * 
     */ 
	uint8_t *decoded_bits = d_decoder.decode(&ofdm, &frame, d_deinterleaved); 

	return parse_signal(decoded_bits);
}

void frame_equalizer_impl::deinterleave(uint8_t *rx_bits) {
	for(int i = 0; i < 48; i++) {
		d_deinterleaved[i] = rx_bits[interleaver_pattern[i]];
	}
}

/*
 * 解析 signal field
 * decoded_bits: 48个 uint，但是每个成员只为 0 或者 1，因为signal 固定为 bpsk 调制
 * signal 由哪些字段组成:https://sampson.blog.csdn.net/article/details/105232288
 * | 0-3bit | 4bit | 5-16bit | 17bit  |  18-23bit  |
 * | Rate	| Res  | Length	 | Parity | Signal tail|
 */ 
bool frame_equalizer_impl::parse_signal(uint8_t *decoded_bits) {

	int r = 0;
	d_frame_bytes = 0;
	bool parity = false;            
	for(int i = 0; i < 17; i++) {
        // 计算奇偶校验码(bit 0~16异或的结果，之后与bit17比较)
		parity ^= decoded_bits[i];

        // rate (0~3 bit)
		if((i < 4) && decoded_bits[i]) {
			r = r | (1 << i);
		}
        
        // length (5~16 bit)
		if(decoded_bits[i] && (i > 4) && (i < 17)) {
			d_frame_bytes = d_frame_bytes | (1 << (i-5));
		}
	}

	if(parity != decoded_bits[17]) {
		dout << "SIGNAL: wrong parity" << std::endl;
		return false;
	}

	switch(r) {
        case 11:
            d_frame_encoding = 0;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 24);
            d_frame_mod = d_bpsk;
            dout << "Encoding: 3 Mbit/s   ";
            break;
        case 15:
            d_frame_encoding = 1;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 36);
            d_frame_mod = d_bpsk;
            dout << "Encoding: 4.5 Mbit/s   ";
            break;
        case 10:
            d_frame_encoding = 2;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 48);
            d_frame_mod = d_qpsk;
            dout << "Encoding: 6 Mbit/s   ";
            break;
        case 14:
            d_frame_encoding = 3;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 72);
            d_frame_mod = d_qpsk;
            dout << "Encoding: 9 Mbit/s   ";
            break;
        case 9:
            d_frame_encoding = 4;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 96);
            d_frame_mod = d_16qam;
            dout << "Encoding: 12 Mbit/s   ";
            break;
        case 13:
            d_frame_encoding = 5;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 144);
            d_frame_mod = d_16qam;
            dout << "Encoding: 18 Mbit/s   ";
            break;
        case 8:
            d_frame_encoding = 6;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 192);
            d_frame_mod = d_64qam;
            dout << "Encoding: 24 Mbit/s   ";
            break;
        case 12:
            d_frame_encoding = 7;
            d_frame_symbols = (int) ceil((16 + 8 * d_frame_bytes + 6) / (double) 216);
            d_frame_mod = d_64qam;
            dout << "Encoding: 27 Mbit/s   ";
            break;
        default:
            dout << "unknown encoding" << std::endl;
            return false;
	}

	mylog(boost::format("encoding: %1% - length: %2% - symbols: %3%")% d_frame_encoding % d_frame_bytes % d_frame_symbols);
	return true;
}

const int
frame_equalizer_impl::interleaver_pattern[48] = {
	 0, 3, 6, 9,12,15,18,21,
	24,27,30,33,36,39,42,45,
	 1, 4, 7,10,13,16,19,22,
	25,28,31,34,37,40,43,46,
	 2, 5, 8,11,14,17,20,23,
	26,29,32,35,38,41,44,47};

} /* namespace ieee802_11 */
} /* namespace gr */
