/*
 * Copyright 2013-2015, Bromium, Inc.
 * Author: Julian Pidancet <julian@pidancet.net>
 * SPDX-License-Identifier: ISC
 */

#ifndef _BASE64_H_
#define _BASE64_H_

unsigned char *base64_decode(const char *input, size_t *output_len);
char *base64_encode(const unsigned char *data, size_t len);

#endif /* _BASE64_H_ */
