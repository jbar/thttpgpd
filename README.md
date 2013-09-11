# thttpgpd and LUDD

thttpgpd or ludd are HTTP servers with OpenPGP features (hkp public key server,
HTTP OpenPGP authentication, Accept: "multipart/msigned" MIME type...).

 The difference between ludd and thttpgpd is that ludd is compiled with OpenUDC
support while thttpgpd is just an HTTP server with OpenPGP features.

 ludd is an experimental implementation node of an OpenUDC monetary
system, which permit :
 - To create a currency and control its monetary mass.
 - To exchange money in this currency between individuals.
 - To apply Universal monetary Dividend to the individuals that accept
  to use the currency.
And all of that with best efforts on security and efficiency.

 About Universal monetary Dividend, please read the Theory of Money
Relativity from Stephane Laborde (in french:
http://www.creationmonetaire.info/2011/06/theorie-relative-de-la-monnaie-20.html)
or at least other studies about Social Credit.

 thttpgpd is a fork/update of thttpd-2.25b
(tiny/turbo/throttling HTTP server), version 2.25b of 29dec2003.

Check the web page (http://www.openudc.org) for updates, or add
yourself to the mailing list openudc@googlegroups.com

## Build Status [![Build Status](https://secure.travis-ci.org/Open-UDC/thttpgpd.png?branch=master)](http://travis-ci.org/Open-UDC/thttpgpd)

## License

Copyright (c) 2010 The openudc.org team.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

The file 'geolist_ITA' is available under the Creative Commons
Attribution 3.0 License.   It was generated from the IT.zip database
from geonames.org, using the script published in
<https://bitbucket.org/rev22/geolist-ita-openudc>.
