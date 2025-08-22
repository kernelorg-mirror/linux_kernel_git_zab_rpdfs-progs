/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef RPDFS_SHARED_VA_H
#define RPDFS_SHARED_VA_H

/*
 * This pleasingly bonkers construct lets the preprocessor calculate the
 * number of variadic args at compile time.  We concatenate the args
 * with a decreasing count from max args (127 for C).  We pass all that
 * to a macro which always returns the 128th arg which will return the
 * decreasing count which matches the number of arguments.
 */
#include <stdio.h>
#include <stdarg.h>

#define VA_NR_ARGS(...) \
	VA_NR_ARGS_(__VA_ARGS__, VA_DECREASING)
#define VA_NR_ARGS_(...) \
	VA_128TH(__VA_ARGS__)
#define VA_128TH( \
          _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
         _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
         _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
         _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
         _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
         _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
         _61,_62,_63,_64,_65,_66,_67,_68,_69,_70, \
         _71,_72,_73,_74,_75,_76,_77,_78,_79,_80, \
         _81,_82,_83,_84,_85,_86,_87,_88,_89,_90, \
         _91,_92,_93,_94,_95,_96,_97,_98,_99,_100, \
         _101,_102,_103,_104,_105,_106,_107,_108,_109,_110, \
         _111,_112,_113,_114,_115,_116,_117,_118,_119,_120, \
         _121,_122,_123,_124,_125,_126,_127,N,...) N
#define VA_DECREASING \
         127,126,125,124,123,122,121,120, \
         119,118,117,116,115,114,113,112,111,110, \
         109,108,107,106,105,104,103,102,101,100, \
         99,98,97,96,95,94,93,92,91,90, \
         89,88,87,86,85,84,83,82,81,80, \
         79,78,77,76,75,74,73,72,71,70, \
         69,68,67,66,65,64,63,62,61,60, \
         59,58,57,56,55,54,53,52,51,50, \
         49,48,47,46,45,44,43,42,41,40, \
         39,38,37,36,35,34,33,32,31,30, \
         29,28,27,26,25,24,23,22,21,20, \
         19,18,17,16,15,14,13,12,11,10, \
         9,8,7,6,5,4,3,2,1,0

#define _VA_CONCAT(A, B)	_VA_CONCAT_(A, B)
#define _VA_CONCAT_(A, B)	A##B

/*
 * Concatenate each argument with a prefix and expand the result.  This
 * does insert commas between the concatenated args.  This is used with
 * arguments that look like macro expansions.
 *
 * VA_CONCAT_EACH(foo_, a(1), a(2)) -> foo_a(1) , foo_a(2)
 */
#define VA_CONCAT_EACH(C, ...) \
	_VA_CONCAT(_VA_CONCAT_EACH_, VA_NR_ARGS(__VA_ARGS__)) (C, __VA_ARGS__)


#define _VA_CONCAT_EACH_0(C, ARG, ...)
#define _VA_CONCAT_EACH_1(C, ARG, ...) _VA_CONCAT(C, ARG)
#define _VA_CONCAT_EACH_2(C, ARG, ...) _VA_CONCAT(C, ARG) , _VA_CONCAT_EACH_1(C, __VA_ARGS__)
#define _VA_CONCAT_EACH_3(C, ARG, ...) _VA_CONCAT(C, ARG) , _VA_CONCAT_EACH_2(C, __VA_ARGS__)
#define _VA_CONCAT_EACH_4(C, ARG, ...) _VA_CONCAT(C, ARG) , _VA_CONCAT_EACH_3(C, __VA_ARGS__)
#define _VA_CONCAT_EACH_5(C, ARG, ...) _VA_CONCAT(C, ARG) , _VA_CONCAT_EACH_4(C, __VA_ARGS__)
#define _VA_CONCAT_EACH_6(C, ARG, ...) _VA_CONCAT(C, ARG) , _VA_CONCAT_EACH_5(C, __VA_ARGS__)

/*
 * Evaluate a macro for each argument and include its argument number as
 * a second argument.
 *
 * VA_FOR_EACH_N(foo, a, b) -> foo(a, 0) foo(b, 1)
 */
#define VA_FOR_EACH_N(M, ...) \
	_VA_CONCAT(_VA_FOR_EACH_N_, VA_NR_ARGS(__VA_ARGS__)) (M, __VA_ARGS__)

/*
 * The first layer passes the 0 based index to the macro numbered by the
 * number of arguments.
 */
#define _VA_FOR_EACH_N_0(M)
#define _VA_FOR_EACH_N_1(M, ...) _VA_FOR_EACH_N_O_1(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_2(M, ...) _VA_FOR_EACH_N_O_2(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_3(M, ...) _VA_FOR_EACH_N_O_3(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_4(M, ...) _VA_FOR_EACH_N_O_4(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_5(M, ...) _VA_FOR_EACH_N_O_5(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_6(M, ...) _VA_FOR_EACH_N_O_6(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_7(M, ...) _VA_FOR_EACH_N_O_7(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_8(M, ...) _VA_FOR_EACH_N_O_8(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_9(M, ...) _VA_FOR_EACH_N_O_9(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_10(M, ...) _VA_FOR_EACH_N_O_10(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_11(M, ...) _VA_FOR_EACH_N_O_11(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_12(M, ...) _VA_FOR_EACH_N_O_12(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_13(M, ...) _VA_FOR_EACH_N_O_13(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_14(M, ...) _VA_FOR_EACH_N_O_14(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_15(M, ...) _VA_FOR_EACH_N_O_15(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_16(M, ...) _VA_FOR_EACH_N_O_16(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_17(M, ...) _VA_FOR_EACH_N_O_17(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_18(M, ...) _VA_FOR_EACH_N_O_18(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_19(M, ...) _VA_FOR_EACH_N_O_19(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_20(M, ...) _VA_FOR_EACH_N_O_20(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_21(M, ...) _VA_FOR_EACH_N_O_21(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_22(M, ...) _VA_FOR_EACH_N_O_22(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_23(M, ...) _VA_FOR_EACH_N_O_23(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_24(M, ...) _VA_FOR_EACH_N_O_24(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_25(M, ...) _VA_FOR_EACH_N_O_25(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_26(M, ...) _VA_FOR_EACH_N_O_26(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_27(M, ...) _VA_FOR_EACH_N_O_27(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_28(M, ...) _VA_FOR_EACH_N_O_28(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_29(M, ...) _VA_FOR_EACH_N_O_29(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_30(M, ...) _VA_FOR_EACH_N_O_30(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_31(M, ...) _VA_FOR_EACH_N_O_31(M, 0, __VA_ARGS__)
#define _VA_FOR_EACH_N_32(M, ...) _VA_FOR_EACH_N_O_32(M, 0, __VA_ARGS__)

/*
 * The rest store their initial argument at the offset, passing the rest
 * of the offsets down the line with the next index.
 */
#define _VA_FOR_EACH_N_O_0(M)
#define _VA_FOR_EACH_N_O_1(M, O, ARG) M(ARG, O)
#define _VA_FOR_EACH_N_O_2(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_1(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_3(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_2(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_4(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_3(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_5(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_4(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_6(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_5(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_7(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_6(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_8(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_7(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_9(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_8(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_10(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_9(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_11(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_10(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_12(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_11(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_13(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_12(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_14(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_13(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_15(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_14(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_16(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_15(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_17(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_16(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_18(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_17(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_19(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_18(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_20(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_19(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_21(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_20(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_22(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_21(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_23(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_22(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_24(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_23(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_25(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_24(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_26(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_25(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_27(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_26(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_28(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_27(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_29(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_28(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_30(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_29(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_31(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_30(M, O + 1, __VA_ARGS__)
#define _VA_FOR_EACH_N_O_32(M, O, ARG, ...) M(ARG, O) _VA_FOR_EACH_N_O_31(M, O + 1, __VA_ARGS__)

#endif
