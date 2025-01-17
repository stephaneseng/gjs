/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright © 2018 Philip Chimento
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <glib.h>

#include "gjs/jsapi-wrapper.h"

#include "test/gjs-test-common.h"

char* gjs_test_get_exception_message(JSContext* cx) {
    if (!JS_IsExceptionPending(cx))
        return nullptr;

    JS::RootedValue v_exc(cx);
    g_assert_true(JS_GetPendingException(cx, &v_exc));
    g_assert_true(v_exc.isObject());

    JS::RootedObject exc(cx, &v_exc.toObject());
    JSErrorReport* report = JS_ErrorFromException(cx, exc);
    g_assert_nonnull(report);

    char* retval = g_strdup(report->message().c_str());
    g_assert_nonnull(retval);
    JS_ClearPendingException(cx);
    return retval;
}
