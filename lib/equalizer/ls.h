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

#ifndef INCLUDED_IEEE802_11_EQUALIZER_LS_H
#define INCLUDED_IEEE802_11_EQUALIZER_LS_H

#include "base.h"
#include <vector>

namespace gr {
namespace ieee802_11 {
namespace equalizer {

/*ls:一种信道估计算法:https://blog.csdn.net/qq_37989552/article/details/102908918 */
class ls: public base {
public:
	virtual void equalize(gr_complex *in, int n, gr_complex *symbols, uint8_t *bits, boost::shared_ptr<gr::digital::constellation> mod);
	virtual double get_snr();
private:
	gr_complex d_H[64];     // Y=HX+N,x是发送信号,Y是接收信号,计算Ｈ就是信道估计
	double d_snr;           // snr:信噪比
};

} /* namespace channel_estimation */
} /* namespace ieee802_11 */
} /* namespace gr */

#endif /* INCLUDED_IEEE802_11_EQUALIZER_LS_H */

