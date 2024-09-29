// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/string.h>

#include <boost/test/unit_test.hpp>
#include <test/util/setup_common.h>

using namespace util;

BOOST_AUTO_TEST_SUITE(util_string_tests)

BOOST_AUTO_TEST_CASE(ConstevalFormatString_Spec)
{
    CheckFormatSpecifiers("", 0);
    CheckFormatSpecifiers("%%", 0);
    CheckFormatSpecifiers("%s", 1);
    CheckFormatSpecifiers("%%s", 0);
    CheckFormatSpecifiers("s%%", 0);
    CheckFormatSpecifiers("%%%s", 1);
    CheckFormatSpecifiers("%s%%", 1);
    CheckFormatSpecifiers(" 1$s", 0);
    CheckFormatSpecifiers("%1$s", 1);
    CheckFormatSpecifiers("%1$s%1$s", 1);
    CheckFormatSpecifiers("%2$s", 2);
    CheckFormatSpecifiers("%2$s 4$s %2$s", 2);
    CheckFormatSpecifiers("%129$s 999$s %2$s", 129);
    CheckFormatSpecifiers("%02d", 1);
    CheckFormatSpecifiers("%+2s", 1);
    CheckFormatSpecifiers("%.6i", 1);
    CheckFormatSpecifiers("%5.2f", 1);
    CheckFormatSpecifiers("%#x", 1);
    CheckFormatSpecifiers("%1$5i", 1);
    CheckFormatSpecifiers("%1$-5i", 1);
    CheckFormatSpecifiers("%1$.5i", 1);
    // tinyformat accepts almost any "type" spec, even '%', or '_', or '\n'.
    CheckFormatSpecifiers("%123%", 1);
    CheckFormatSpecifiers("%123%s", 1);
    CheckFormatSpecifiers("%_", 1);
    CheckFormatSpecifiers("%\n", 1);

    // The `*` specifier behavior is unsupported and can lead to runtime
    // errors when used in a ConstevalFormatString. Please refer to the
    // note in the ConstevalFormatString docs.
    CheckFormatSpecifiers("%2$*3$d", 2);
    CheckFormatSpecifiers("%.*f", 1);

    HasReason err_mix{"Format specifiers must be all positional or all non-positional!"};
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%s%1$s", 1), const char*, err_mix);

    HasReason err_num{"Format specifier count must match the argument count!"};
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("", 1), const char*, err_num);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%s", 0), const char*, err_num);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%s", 2), const char*, err_num);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%1$s", 0), const char*, err_num);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%1$s", 2), const char*, err_num);

    HasReason err_0_pos{"Positional format specifier must have position of at least 1!"};
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%$s", -1), const char*, err_0_pos);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%$", -1), const char*, err_0_pos);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%0$", -1), const char*, err_0_pos);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%0$s", -1), const char*, err_0_pos);

    HasReason err_term{"Format specifier incorrectly terminated by end of string!"};
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%", -1), const char*, err_term);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%1", -1), const char*, err_term);
    BOOST_CHECK_EXCEPTION(CheckFormatSpecifiers("%1$", -1), const char*, err_term);
}

BOOST_AUTO_TEST_CASE(ConstevalFormatString_SpecificUsageTests)
{
    // Example usages from bitcoin-cli
    CheckFormatSpecifiers("<->   type   net  v  mping   ping send recv  txn  blk  hb %*s%*s%*s ", 6);
    CheckFormatSpecifiers("%*s %-*s%s\n", 5);
    CheckFormatSpecifiers("%3s %6s %5s %2s%7s%7s%5s%5s%5s%5s  %2s %*s%*s%*s%*i %*s %-*s%s\n", 24);
    CheckFormatSpecifiers("                        ms     ms  sec  sec  min  min                %*s\n\n", 2);
    CheckFormatSpecifiers("\n%-*s    port %6i    score %6i", 4);
    CheckFormatSpecifiers("%*s %s\n", 3);
}

BOOST_AUTO_TEST_SUITE_END()
