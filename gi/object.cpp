/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil; -*- */
/*
 * Copyright (c) 2008  litl, LLC
 * Copyright (c) 2018  Philip Chimento <philip.chimento@gmail.com>
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

#include <string.h>  // for memset, strcmp

#include <algorithm>   // for move, find
#include <functional>  // for mem_fn
#include <string>
#include <tuple>        // for tie
#include <type_traits>  // for remove_reference<>::type
#include <vector>

#include <ffi.h>
#include <girepository.h>
#include <glib-object.h>
#include <glib.h>

#include "gjs/jsapi-wrapper.h"

#include "gi/arg.h"
#include "gi/closure.h"
#include "gi/function.h"
#include "gi/gjs_gi_trace.h"
#include "gi/object.h"
#include "gi/repo.h"
#include "gi/toggle.h"
#include "gi/value.h"
#include "gi/wrapperutils.h"
#include "gjs/atoms.h"
#include "gjs/context-private.h"
#include "gjs/context.h"
#include "gjs/jsapi-class.h"
#include "gjs/jsapi-util-args.h"
#include "gjs/jsapi-util-root.h"
#include "gjs/mem-private.h"
#include "util/log.h"

/* This is a trick to print out the sizes of the structs at compile time, in
 * an error message. */
// template <int s> struct Measure;
// Measure<sizeof(ObjectInstance)> instance_size;
// Measure<sizeof(ObjectPrototype)> prototype_size;

#if defined(__x86_64__) && defined(__clang__)
/* This isn't meant to be comprehensive, but should trip on at least one CI job
 * if sizeof(ObjectInstance) is increased. */
static_assert(sizeof(ObjectInstance) <= 88,
              "Think very hard before increasing the size of ObjectInstance. "
              "There can be tens of thousands of them alive in a typical "
              "gnome-shell run.");
#endif  // x86-64 clang

bool ObjectInstance::s_weak_pointer_callback = false;
ObjectInstance *ObjectInstance::wrapped_gobject_list = nullptr;

// clang-format off
G_DEFINE_QUARK(gjs::custom-type, ObjectBase::custom_type)
G_DEFINE_QUARK(gjs::custom-property, ObjectBase::custom_property)
// clang-format on

GJS_USE
static GQuark
gjs_object_priv_quark (void)
{
    static GQuark val = 0;
    if (G_UNLIKELY (!val))
        val = g_quark_from_static_string ("gjs::private");

    return val;
}

/* Plain g_type_query fails and leaves @query uninitialized for
   dynamic types.
   See https://bugzilla.gnome.org/show_bug.cgi?id=687184 and
   https://bugzilla.gnome.org/show_bug.cgi?id=687211
*/
void ObjectBase::type_query_dynamic_safe(GTypeQuery* query) {
    GType type = gtype();
    while (g_type_get_qdata(type, ObjectBase::custom_type_quark()))
        type = g_type_parent(type);

    g_type_query(type, query);
}

void
GjsListLink::prepend(ObjectInstance *this_instance,
                     ObjectInstance *head)
{
    GjsListLink *elem = head->get_link();

    g_assert(this_instance->get_link() == this);

    if (elem->m_prev) {
        GjsListLink *prev = elem->m_prev->get_link();
        prev->m_next = this_instance;
        this->m_prev = elem->m_prev;
    }

    elem->m_prev = this_instance;
    this->m_next = head;
}

void
GjsListLink::unlink(void)
{
    if (m_prev)
        m_prev->get_link()->m_next = m_next;
    if (m_next)
        m_next->get_link()->m_prev = m_prev;

    m_prev = m_next = nullptr;
}

size_t
GjsListLink::size(void) const
{
    const GjsListLink *elem = this;
    size_t count = 0;

    do {
        count++;
        if (!elem->m_next)
            break;
        elem = elem->m_next->get_link();
    } while (elem);

    return count;
}

void ObjectInstance::link(void) {
    if (wrapped_gobject_list)
        m_instance_link.prepend(this, wrapped_gobject_list);
    wrapped_gobject_list = this;
}

void ObjectInstance::unlink(void) {
    if (wrapped_gobject_list == this)
        wrapped_gobject_list = m_instance_link.next();
    m_instance_link.unlink();
}

const JSObject* ObjectBase::jsobj_addr(void) const {
    if (is_prototype())
        return nullptr;
    return to_instance()->wrapper();
}

// Overrides GIWrapperBase::typecheck(). We only override the overload that
// throws, so that we can throw our own more informative error.
bool ObjectBase::typecheck(JSContext* cx, JS::HandleObject obj,
                           GIObjectInfo* expected_info, GType expected_gtype) {
    if (GIWrapperBase::typecheck(cx, obj, expected_info, expected_gtype))
        return true;

    gjs_throw(cx,
              "This JS object wrapper isn't wrapping a GObject."
              " If this is a custom subclass, are you sure you chained"
              " up to the parent _init properly?");
    return false;
}

bool ObjectInstance::check_gobject_disposed(const char* for_what) const {
    if (!m_gobj_disposed)
        return true;

    g_critical(
        "Object %s.%s (%p), has been already deallocated — impossible to %s "
        "it. This might be caused by the object having been destroyed from C "
        "code using something such as destroy(), dispose(), or remove() "
        "vfuncs.",
        ns(), name(), m_ptr, for_what);
    gjs_dumpstack();
    return false;
}

ObjectInstance *
ObjectInstance::for_gobject(GObject *gobj)
{
    auto priv = static_cast<ObjectInstance *>(g_object_get_qdata(gobj,
                                                                 gjs_object_priv_quark()));

    if (priv)
        priv->check_js_object_finalized();

    return priv;
}

void
ObjectInstance::check_js_object_finalized(void)
{
    if (!m_uses_toggle_ref)
        return;
    if (G_UNLIKELY(m_wrapper_finalized)) {
        g_critical(
            "Object %p (a %s) resurfaced after the JS wrapper was finalized. "
            "This is some library doing dubious memory management inside "
            "dispose()",
            m_ptr, type_name());
        m_wrapper_finalized = false;
        g_assert(!m_wrapper);  /* should associate again with a new wrapper */
    }
}

ObjectPrototype* ObjectPrototype::for_gtype(GType gtype) {
    return static_cast<ObjectPrototype*>(
        g_type_get_qdata(gtype, gjs_object_priv_quark()));
}

void ObjectPrototype::set_type_qdata(void) {
    g_type_set_qdata(m_gtype, gjs_object_priv_quark(), this);
}

void
ObjectInstance::set_object_qdata(void)
{
    g_object_set_qdata(m_ptr, gjs_object_priv_quark(), this);
}

void
ObjectInstance::unset_object_qdata(void)
{
    g_object_set_qdata(m_ptr, gjs_object_priv_quark(), nullptr);
}

GParamSpec* ObjectPrototype::find_param_spec_from_id(JSContext* cx,
                                                     JS::HandleString key) {
    /* First check for the ID in the cache */
    auto entry = m_property_cache.lookupForAdd(key);
    if (entry)
        return entry->value();

    JS::UniqueChars js_prop_name(JS_EncodeStringToUTF8(cx, key));
    if (!js_prop_name)
        return nullptr;

    GjsAutoChar gname = gjs_hyphen_from_camel(js_prop_name.get());
    GjsAutoTypeClass<GObjectClass> gobj_class(m_gtype);
    GParamSpec* pspec = g_object_class_find_property(gobj_class, gname);
    GjsAutoParam param_spec(pspec, GjsAutoTakeOwnership());

    if (!param_spec) {
        gjs_wrapper_throw_nonexistent_field(cx, m_gtype, js_prop_name.get());
        return nullptr;
    }

    if (!m_property_cache.add(entry, key, std::move(param_spec))) {
        JS_ReportOutOfMemory(cx);
        return nullptr;
    }
    return pspec; /* owned by property cache */
}

/* A hook on adding a property to an object. This is called during a set
 * property operation after all the resolve hooks on the prototype chain have
 * failed to resolve. We use this to mark an object as needing toggle refs when
 * custom state is set on it, because we need to keep the JS GObject wrapper
 * alive in order not to lose custom "expando" properties.
 */
bool ObjectBase::add_property(JSContext* cx, JS::HandleObject obj,
                              JS::HandleId id, JS::HandleValue value) {
    auto* priv = ObjectBase::for_js(cx, obj);

    /* priv is null during init: property is not being added from JS */
    if (!priv) {
        debug_jsprop_static("Add property hook", id, obj);
        return true;
    }
    if (priv->is_prototype())
        return true;

    return priv->to_instance()->add_property_impl(cx, obj, id, value);
}

bool ObjectInstance::add_property_impl(JSContext* cx, JS::HandleObject obj,
                                       JS::HandleId id, JS::HandleValue) {
    debug_jsprop("Add property hook", id, obj);

    if (is_custom_js_class() || m_gobj_disposed)
        return true;

    ensure_uses_toggle_ref(cx);
    return true;
}

bool ObjectBase::prop_getter(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);

    JS::RootedString name(cx,
        gjs_dynamic_property_private_slot(&args.callee()).toString());

    priv->debug_jsprop("Property getter", name, obj);

    if (priv->is_prototype())
        return true;
        /* Ignore silently; note that this is different from what we do for
         * boxed types, for historical reasons */

    return priv->to_instance()->prop_getter_impl(cx, name, args.rval());
}

bool ObjectInstance::prop_getter_impl(JSContext* cx, JS::HandleString name,
                                      JS::MutableHandleValue rval) {
    if (!check_gobject_disposed("get any property from"))
        return true;

    GValue gvalue = { 0, };

    ObjectPrototype* proto_priv = get_prototype();
    GParamSpec *param = proto_priv->find_param_spec_from_id(cx, name);

    /* This is guaranteed because we resolved the property before */
    g_assert(param);

    /* Do not fetch JS overridden properties from GObject, to avoid
     * infinite recursion. */
    if (g_param_spec_get_qdata(param, ObjectInstance::custom_property_quark()))
        return true;

    if ((param->flags & G_PARAM_READABLE) == 0) {
        rval.setUndefined();
        return true;
    }

    gjs_debug_jsprop(GJS_DEBUG_GOBJECT, "Accessing GObject property %s",
                     param->name);

    g_value_init(&gvalue, G_PARAM_SPEC_VALUE_TYPE(param));
    g_object_get_property(m_ptr, param->name, &gvalue);
    if (!gjs_value_from_g_value(cx, rval, &gvalue)) {
        g_value_unset(&gvalue);
        return false;
    }
    g_value_unset(&gvalue);

    return true;
}

GJS_USE
static GjsAutoFieldInfo lookup_field_info(GIObjectInfo* info,
                                          const char* name) {
    int n_fields = g_object_info_get_n_fields(info);
    int ix;
    GjsAutoFieldInfo retval;

    for (ix = 0; ix < n_fields; ix++) {
        retval = g_object_info_get_field(info, ix);
        if (strcmp(name, retval.name()) == 0)
            break;
        retval.reset();
    }

    if (!retval || !(g_field_info_get_flags(retval) & GI_FIELD_IS_READABLE))
        return nullptr;

    return retval;
}

bool ObjectBase::field_getter(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);

    JS::RootedString name(cx,
        gjs_dynamic_property_private_slot(&args.callee()).toString());

    priv->debug_jsprop("Field getter", name, obj);

    if (priv->is_prototype())
        return true;
        /* Ignore silently; note that this is different from what we do for
         * boxed types, for historical reasons */

    return priv->to_instance()->field_getter_impl(cx, name, args.rval());
}

bool ObjectInstance::field_getter_impl(JSContext* cx, JS::HandleString name,
                                       JS::MutableHandleValue rval) {
    if (!check_gobject_disposed("get any property from"))
        return true;

    ObjectPrototype* proto_priv = get_prototype();
    GIFieldInfo* field = proto_priv->lookup_cached_field_info(cx, name);
    GITypeTag tag;
    GIArgument arg = { 0 };

    gjs_debug_jsprop(GJS_DEBUG_GOBJECT, "Overriding %s with GObject field",
                     gjs_debug_string(name).c_str());

    GjsAutoTypeInfo type = g_field_info_get_type(field);
    tag = g_type_info_get_tag(type);
    if (tag == GI_TYPE_TAG_ARRAY ||
        tag == GI_TYPE_TAG_INTERFACE ||
        tag == GI_TYPE_TAG_GLIST ||
        tag == GI_TYPE_TAG_GSLIST ||
        tag == GI_TYPE_TAG_GHASH ||
        tag == GI_TYPE_TAG_ERROR) {
        gjs_throw(cx, "Can't get field %s; GObject introspection supports only "
                  "fields with simple types, not %s",
                  gjs_debug_string(name).c_str(), g_type_tag_to_string(tag));
        return false;
    }

    if (!g_field_info_get_field(field, m_ptr, &arg)) {
        gjs_throw(cx, "Error getting field %s from object",
                  gjs_debug_string(name).c_str());
        return false;
    }

    return gjs_value_from_g_argument(cx, rval, type, &arg, true);
    /* copy_structs is irrelevant because g_field_info_get_field() doesn't
     * handle boxed types */
}

/* Dynamic setter for GObject properties. Returns false on OOM/exception.
 * args.rval() becomes the "stored value" for the property. */
bool ObjectBase::prop_setter(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);

    JS::RootedString name(cx,
        gjs_dynamic_property_private_slot(&args.callee()).toString());

    priv->debug_jsprop("Property setter", name, obj);

    if (priv->is_prototype())
        return true;
        /* Ignore silently; note that this is different from what we do for
         * boxed types, for historical reasons */

    /* Clear the JS stored value, to avoid keeping additional references */
    args.rval().setUndefined();

    return priv->to_instance()->prop_setter_impl(cx, name, args[0]);
}

bool ObjectInstance::prop_setter_impl(JSContext* cx, JS::HandleString name,
                                      JS::HandleValue value) {
    if (!check_gobject_disposed("set any property on"))
        return true;

    ObjectPrototype* proto_priv = get_prototype();
    GParamSpec *param_spec = proto_priv->find_param_spec_from_id(cx, name);
    if (!param_spec)
        return false;

    /* Do not set JS overridden properties through GObject, to avoid
     * infinite recursion (unless constructing) */
    if (g_param_spec_get_qdata(param_spec,
        ObjectInstance::custom_property_quark()))
        return true;

    if (!(param_spec->flags & G_PARAM_WRITABLE))
        /* prevent setting the prop even in JS */
        return gjs_wrapper_throw_readonly_field(cx, gtype(), param_spec->name);

    gjs_debug_jsprop(GJS_DEBUG_GOBJECT, "Setting GObject prop %s",
                     param_spec->name);

    GValue gvalue = G_VALUE_INIT;
    g_value_init(&gvalue, G_PARAM_SPEC_VALUE_TYPE(param_spec));
    if (!gjs_value_to_g_value(cx, value, &gvalue)) {
        g_value_unset(&gvalue);
        return false;
    }

    g_object_set_property(m_ptr, param_spec->name, &gvalue);
    g_value_unset(&gvalue);

    return true;
}

bool ObjectBase::field_setter(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);

    JS::RootedString name(cx,
        gjs_dynamic_property_private_slot(&args.callee()).toString());

    priv->debug_jsprop("Field setter", name, obj);

    if (priv->is_prototype())
        return true;
        /* Ignore silently; note that this is different from what we do for
         * boxed types, for historical reasons */

    /* We have to update args.rval(), because JS caches it as the property's "stored
     * value" (https://developer.mozilla.org/en-US/docs/Mozilla/Projects/SpiderMonkey/JSAPI_reference/Stored_value)
     * and so subsequent gets would get the stored value instead of accessing
     * the field */
    args.rval().setUndefined();

    return priv->to_instance()->field_setter_not_impl(cx, name);
}

bool ObjectInstance::field_setter_not_impl(JSContext* cx,
                                           JS::HandleString name) {
    if (!check_gobject_disposed("set GObject field on"))
        return true;

    ObjectPrototype* proto_priv = get_prototype();
    GIFieldInfo* field = proto_priv->lookup_cached_field_info(cx, name);

    /* As far as I know, GI never exposes GObject instance struct fields as
     * writable, so no need to implement this for the time being */
    if (g_field_info_get_flags(field) & GI_FIELD_IS_WRITABLE) {
        g_message("Field %s of a GObject is writable, but setting it is not "
                  "implemented", gjs_debug_string(name).c_str());
        return true;
    }

    return gjs_wrapper_throw_readonly_field(cx, gtype(),
                                            g_base_info_get_name(field));
}

bool ObjectPrototype::is_vfunc_unchanged(GIVFuncInfo* info) {
    GType ptype = g_type_parent(m_gtype);
    GError *error = NULL;
    gpointer addr1, addr2;

    addr1 = g_vfunc_info_get_address(info, m_gtype, &error);
    if (error) {
        g_clear_error(&error);
        return false;
    }

    addr2 = g_vfunc_info_get_address(info, ptype, &error);
    if (error) {
        g_clear_error(&error);
        return false;
    }

    return addr1 == addr2;
}

GJS_USE
static GjsAutoVFuncInfo find_vfunc_on_parents(GIObjectInfo* info,
                                              const char* name,
                                              bool* out_defined_by_parent) {
    bool defined_by_parent = false;

    /* ref the first info so that we don't destroy
     * it when unrefing parents later */
    GjsAutoObjectInfo parent = g_base_info_ref(info);

    /* Since it isn't possible to override a vfunc on
     * an interface without reimplementing it, we don't need
     * to search the parent types when looking for a vfunc. */
    GjsAutoVFuncInfo vfunc =
        g_object_info_find_vfunc_using_interfaces(parent, name, nullptr);
    while (!vfunc && parent) {
        parent = g_object_info_get_parent(parent);
        if (parent)
            vfunc = g_object_info_find_vfunc(parent, name);

        defined_by_parent = true;
    }

    if (out_defined_by_parent)
        *out_defined_by_parent = defined_by_parent;

    return vfunc;
}

/* Taken from GLib */
static void canonicalize_key(const GjsAutoChar& key) {
    for (char* p = key.get(); *p != 0; p++) {
        char c = *p;

        if (c != '-' && (c < '0' || c > '9') && (c < 'A' || c > 'Z') &&
            (c < 'a' || c > 'z'))
            *p = '-';
    }
}

/* @name must already be canonicalized */
GJS_USE
static bool is_ginterface_property_name(GIInterfaceInfo* info,
                                        const char* name) {
    int n_props = g_interface_info_get_n_properties(info);
    GjsAutoPropertyInfo prop_info;

    for (int ix = 0; ix < n_props; ix++) {
        prop_info = g_interface_info_get_property(info, ix);
        if (strcmp(name, prop_info.name()) == 0)
            break;
        prop_info.reset();
    }

    return !!prop_info;
}

bool ObjectPrototype::lazy_define_gobject_property(JSContext* cx,
                                                   JS::HandleObject obj,
                                                   JS::HandleId id,
                                                   bool* resolved,
                                                   const char* name) {
    bool found = false;
    if (!JS_AlreadyHasOwnPropertyById(cx, obj, id, &found))
        return false;
    if (found) {
        /* Already defined, so *resolved = false because we didn't just
         * define it */
        *resolved = false;
        return true;
    }

    debug_jsprop("Defining lazy GObject property", id, obj);

    JS::RootedValue private_id(cx, JS::StringValue(JSID_TO_STRING(id)));
    if (!gjs_define_property_dynamic(
            cx, obj, name, "gobject_prop", &ObjectBase::prop_getter,
            &ObjectBase::prop_setter, private_id,
            // Make property configurable so that interface properties can be
            // overridden by GObject.ParamSpec.override in the class that
            // implements them
            GJS_MODULE_PROP_FLAGS & ~JSPROP_PERMANENT))
        return false;

    *resolved = true;
    return true;
}

bool ObjectPrototype::resolve_no_info(JSContext* cx, JS::HandleObject obj,
                                      JS::HandleId id, bool* resolved,
                                      const char* name,
                                      ResolveWhat resolve_props) {
    guint n_interfaces;
    guint i;

    GjsAutoChar canonical_name;
    if (resolve_props == ConsiderMethodsAndProperties) {
        canonical_name = gjs_hyphen_from_camel(name);
        canonicalize_key(canonical_name);
    }

    GType *interfaces = g_type_interfaces(m_gtype, &n_interfaces);
    for (i = 0; i < n_interfaces; i++) {
        GjsAutoInterfaceInfo iface_info =
            g_irepository_find_by_gtype(nullptr, interfaces[i]);
        if (!iface_info)
            continue;

        GjsAutoFunctionInfo method_info =
            g_interface_info_find_method(iface_info, name);
        if (method_info) {
            if (g_function_info_get_flags (method_info) & GI_FUNCTION_IS_METHOD) {
                if (!gjs_define_function(cx, obj, m_gtype, method_info)) {
                    g_free(interfaces);
                    return false;
                }

                *resolved = true;
                g_free(interfaces);
                return true;
            }
        }

        if (resolve_props == ConsiderOnlyMethods)
            continue;

        /* If the name refers to a GObject property, lazily define the property
         * in JS as we do below in the real resolve hook. We ignore fields here
         * because I don't think interfaces can have fields */
        if (is_ginterface_property_name(iface_info, canonical_name)) {
            g_free(interfaces);
            return lazy_define_gobject_property(cx, obj, id, resolved, name);
        }
    }

    *resolved = false;
    g_free(interfaces);
    return true;
}

GJS_USE
static bool
is_gobject_property_name(GIObjectInfo *info,
                         const char   *name)
{
    int n_props = g_object_info_get_n_properties(info);
    int n_ifaces = g_object_info_get_n_interfaces(info);
    int ix;
    GjsAutoPropertyInfo prop_info;

    GjsAutoChar canonical_name = gjs_hyphen_from_camel(name);
    canonicalize_key(canonical_name);

    for (ix = 0; ix < n_props; ix++) {
        prop_info = g_object_info_get_property(info, ix);
        if (strcmp(canonical_name, prop_info.name()) == 0)
            break;
        prop_info.reset();
    }

    if (!prop_info) {
        for (ix = 0; ix < n_ifaces; ix++) {
            GjsAutoInterfaceInfo iface_info =
                g_object_info_get_interface(info, ix);
            if (is_ginterface_property_name(iface_info, canonical_name))
                return true;
        }
        return false;
    }

    return true;
}

// Override of GIWrapperBase::id_is_never_lazy()
bool ObjectBase::id_is_never_lazy(jsid name, const GjsAtoms& atoms) {
    // Keep this list in sync with ObjectBase::proto_properties and
    // ObjectBase::proto_methods. However, explicitly do not include
    // connect() in it, because there are a few cases where the lazy property
    // should override the predefined one, such as Gio.Cancellable.connect().
    return name == atoms.init() || name == atoms.connect_after() ||
           name == atoms.emit();
}

bool ObjectPrototype::resolve_impl(JSContext* context, JS::HandleObject obj,
                                   JS::HandleId id, const char* name,
                                   bool* resolved) {
    /* If we have no GIRepository information (we're a JS GObject subclass),
     * we need to look at exposing interfaces. Look up our interfaces through
     * GType data, and then hope that *those* are introspectable. */
    if (is_custom_js_class())
        return resolve_no_info(context, obj, id, resolved, name,
                               ConsiderMethodsAndProperties);

    if (g_str_has_prefix(name, "vfunc_")) {
        /* The only time we find a vfunc info is when we're the base
         * class that defined the vfunc. If we let regular prototype
         * chaining resolve this, we'd have the implementation for the base's
         * vfunc on the base class, without any other "real" implementations
         * in the way. If we want to expose a "real" vfunc implementation,
         * we need to go down to the parent infos and look at their VFuncInfos.
         *
         * This is good, but it's memory-hungry -- we would define every
         * possible vfunc on every possible object, even if it's the same
         * "real" vfunc underneath. Instead, only expose vfuncs that are
         * different from their parent, and let prototype chaining do the
         * rest.
         */

        const char *name_without_vfunc_ = &(name[6]);  /* lifetime tied to name */
        bool defined_by_parent;
        GjsAutoVFuncInfo vfunc = find_vfunc_on_parents(
            m_info, name_without_vfunc_, &defined_by_parent);
        if (vfunc) {
            /* In the event that the vfunc is unchanged, let regular
             * prototypal inheritance take over. */
            if (defined_by_parent && is_vfunc_unchanged(vfunc)) {
                *resolved = false;
                return true;
            }

            if (!gjs_define_function(context, obj, m_gtype, vfunc))
                return false;

            *resolved = true;
            return true;
        }

        /* If the vfunc wasn't found, fall through, back to normal
         * method resolution. */
    }

    if (is_gobject_property_name(m_info, name))
        return lazy_define_gobject_property(context, obj, id, resolved, name);

    GjsAutoFieldInfo field_info = lookup_field_info(m_info, name);
    if (field_info) {
        bool found = false;
        if (!JS_AlreadyHasOwnPropertyById(context, obj, id, &found))
            return false;
        if (found) {
            *resolved = false;
            return true;
        }

        debug_jsprop("Defining lazy GObject field", id, obj);

        unsigned flags = GJS_MODULE_PROP_FLAGS;
        if (!(g_field_info_get_flags(field_info) & GI_FIELD_IS_WRITABLE))
            flags |= JSPROP_READONLY;

        JS::RootedString key(context, JSID_TO_STRING(id));
        if (!m_field_cache.putNew(key, field_info.release())) {
            JS_ReportOutOfMemory(context);
            return false;
        }

        JS::RootedValue private_id(context, JS::StringValue(key));
        if (!gjs_define_property_dynamic(
                context, obj, name, "gobject_field", &ObjectBase::field_getter,
                &ObjectBase::field_setter, private_id, flags))
            return false;

        *resolved = true;
        return true;
    }

    /* find_method does not look at methods on parent classes,
     * we rely on javascript to walk up the __proto__ chain
     * and find those and define them in the right prototype.
     *
     * Note that if it isn't a method on the object, since JS
     * lacks multiple inheritance, we're sticking the iface
     * methods in the object prototype, which means there are many
     * copies of the iface methods (one per object class node that
     * introduces the iface)
     */

    GjsAutoFunctionInfo method_info =
        g_object_info_find_method_using_interfaces(m_info, name, nullptr);

    /**
     * Search through any interfaces implemented by the GType;
     * this could be done better.  See
     * https://bugzilla.gnome.org/show_bug.cgi?id=632922
     */
    if (!method_info)
        return resolve_no_info(context, obj, id, resolved, name,
                               ConsiderOnlyMethods);

#if GJS_VERBOSE_ENABLE_GI_USAGE
    _gjs_log_info_usage(method_info);
#endif

    if (g_function_info_get_flags (method_info) & GI_FUNCTION_IS_METHOD) {
        gjs_debug(GJS_DEBUG_GOBJECT,
                  "Defining method %s in prototype for %s (%s.%s)",
                  method_info.name(), type_name(), ns(), this->name());

        if (!gjs_define_function(context, obj, m_gtype, method_info))
            return false;

        *resolved = true; /* we defined the prop in obj */
    } else {
        *resolved = false;
    }

    return true;
}

bool ObjectPrototype::new_enumerate_impl(JSContext* cx, JS::HandleObject,
                                         JS::AutoIdVector& properties,
                                         bool only_enumerable G_GNUC_UNUSED) {
    unsigned n_interfaces;
    GType* interfaces = g_type_interfaces(gtype(), &n_interfaces);

    for (unsigned k = 0; k < n_interfaces; k++) {
        GjsAutoInterfaceInfo iface_info =
            g_irepository_find_by_gtype(nullptr, interfaces[k]);

        if (!iface_info) {
            continue;
        }

        // Methods
        int n_methods = g_interface_info_get_n_methods(iface_info);
        for (int i = 0; i < n_methods; i++) {
            GjsAutoFunctionInfo meth_info =
                g_interface_info_get_method(iface_info, i);
            GIFunctionInfoFlags flags = g_function_info_get_flags(meth_info);

            if (flags & GI_FUNCTION_IS_METHOD) {
                const char* name = meth_info.name();
                jsid id = gjs_intern_string_to_id(cx, name);
                if (id == JSID_VOID)
                    return false;
                if (!properties.append(id)) {
                    JS_ReportOutOfMemory(cx);
                    return false;
                }
            }
        }

        // Properties
        int n_properties = g_interface_info_get_n_properties(iface_info);
        for (int i = 0; i < n_properties; i++) {
            GjsAutoPropertyInfo prop_info =
                g_interface_info_get_property(iface_info, i);

            GjsAutoChar js_name = gjs_hyphen_to_underscore(prop_info.name());

            jsid id = gjs_intern_string_to_id(cx, js_name);
            if (id == JSID_VOID)
                return false;
            if (!properties.append(id)) {
                JS_ReportOutOfMemory(cx);
                return false;
            }
        }
    }

    g_free(interfaces);

    if (!is_custom_js_class()) {
        // Methods
        int n_methods = g_object_info_get_n_methods(info());
        for (int i = 0; i < n_methods; i++) {
            GjsAutoFunctionInfo meth_info = g_object_info_get_method(info(), i);
            GIFunctionInfoFlags flags = g_function_info_get_flags(meth_info);

            if (flags & GI_FUNCTION_IS_METHOD) {
                const char* name = meth_info.name();
                jsid id = gjs_intern_string_to_id(cx, name);
                if (id == JSID_VOID)
                    return false;
                if (!properties.append(id)) {
                    JS_ReportOutOfMemory(cx);
                    return false;
                }
            }
        }

        // Properties
        int n_properties = g_object_info_get_n_properties(info());
        for (int i = 0; i < n_properties; i++) {
            GjsAutoPropertyInfo prop_info =
                g_object_info_get_property(info(), i);

            GjsAutoChar js_name = gjs_hyphen_to_underscore(prop_info.name());
            jsid id = gjs_intern_string_to_id(cx, js_name);
            if (id == JSID_VOID)
                return false;
            if (!properties.append(id)) {
                JS_ReportOutOfMemory(cx);
                return false;
            }
        }
    }

    return true;
}


/* Set properties from args to constructor (args[0] is supposed to be
 * a hash) */
bool ObjectPrototype::props_to_g_parameters(JSContext* context,
                                            const JS::HandleValueArray& args,
                                            std::vector<const char*>* names,
                                            AutoGValueVector* values) {
    size_t ix, length;

    if (args.length() == 0 || args[0].isUndefined())
        return true;

    if (!args[0].isObject()) {
        gjs_throw(context, "argument should be a hash with props to set");
        return false;
    }

    JS::RootedObject props(context, &args[0].toObject());
    JS::RootedId prop_id(context);
    JS::RootedValue value(context);
    JS::Rooted<JS::IdVector> ids(context, context);
    if (!JS_Enumerate(context, props, &ids)) {
        gjs_throw(context, "Failed to create property iterator for object props hash");
        return false;
    }

    for (ix = 0, length = ids.length(); ix < length; ix++) {
        GValue gvalue = G_VALUE_INIT;

        /* ids[ix] is reachable because props is rooted, but require_property
         * doesn't know that */
        prop_id = ids[ix];

        if (!JSID_IS_STRING(prop_id))
            return gjs_wrapper_throw_nonexistent_field(
                context, m_gtype, gjs_debug_id(prop_id).c_str());

        JS::RootedString js_prop_name(context, JSID_TO_STRING(prop_id));
        GParamSpec *param_spec = find_param_spec_from_id(context, js_prop_name);
        if (!param_spec)
            return false;

        if (!JS_GetPropertyById(context, props, prop_id, &value))
            return false;
        if (value.isUndefined()) {
            gjs_throw(context, "Invalid value 'undefined' for property %s in "
                      "object initializer.", param_spec->name);
            return false;
        }

        if (!(param_spec->flags & G_PARAM_WRITABLE))
            return gjs_wrapper_throw_readonly_field(context, m_gtype,
                                                    param_spec->name);
            /* prevent setting the prop even in JS */

        g_value_init(&gvalue, G_PARAM_SPEC_VALUE_TYPE(param_spec));
        if (!gjs_value_to_g_value(context, value, &gvalue)) {
            g_value_unset(&gvalue);
            return false;
        }

        names->push_back(param_spec->name);  /* owned by GParamSpec in cache */
        values->push_back(gvalue);
    }

    return true;
}

static void wrapped_gobj_dispose_notify(
    void* data, GObject* where_the_object_was GJS_USED_VERBOSE_LIFECYCLE) {
    auto *priv = static_cast<ObjectInstance *>(data);
    priv->gobj_dispose_notify();
    gjs_debug_lifecycle(GJS_DEBUG_GOBJECT, "Wrapped GObject %p disposed",
                        where_the_object_was);
}

void
ObjectInstance::gobj_dispose_notify(void)
{
    m_gobj_disposed = true;
    unlink();
}

void
ObjectInstance::iterate_wrapped_gobjects(ObjectInstance::Action action)
{
    ObjectInstance *link = ObjectInstance::wrapped_gobject_list;
    while (link) {
        ObjectInstance *next = link->next();
        action(link);
        link = next;
    }
}

void
ObjectInstance::remove_wrapped_gobjects_if(ObjectInstance::Predicate predicate,
                                           ObjectInstance::Action action)
{
    std::vector<ObjectInstance *> removed;
    iterate_wrapped_gobjects([&](ObjectInstance *link) {
        if (predicate(link)) {
            removed.push_back(link);
            link->unlink();
        }
    });

    for (ObjectInstance *priv : removed)
        action(priv);
}

/*
 * ObjectInstance::context_dispose_notify:
 *
 * Callback called when the #GjsContext is disposed. It just calls
 * handle_context_dispose() on every ObjectInstance.
 */
void ObjectInstance::context_dispose_notify(
    void*, GObject* where_the_object_was G_GNUC_UNUSED) {
    ObjectInstance::iterate_wrapped_gobjects(
        std::mem_fn(&ObjectInstance::handle_context_dispose));
}

/*
 * ObjectInstance::handle_context_dispose:
 *
 * Called on each existing ObjectInstance when the #GjsContext is disposed.
 */
void ObjectInstance::handle_context_dispose(void) {
    if (wrapper_is_rooted()) {
        debug_lifecycle("Was rooted, but unrooting due to GjsContext dispose");
        discard_wrapper();
        unlink();
    }
}

void
ObjectInstance::toggle_down(void)
{
    debug_lifecycle("Toggle notify DOWN");

    /* Change to weak ref so the wrapper-wrappee pair can be
     * collected by the GC
     */
    if (wrapper_is_rooted()) {
        debug_lifecycle("Unrooting wrapper");
        switch_to_unrooted();

        /* During a GC, the collector asks each object which other
         * objects that it wants to hold on to so if there's an entire
         * section of the heap graph that's not connected to anything
         * else, and not reachable from the root set, then it can be
         * trashed all at once.
         *
         * GObjects, however, don't work like that, there's only a
         * reference count but no notion of who owns the reference so,
         * a JS object that's wrapping a GObject is unconditionally held
         * alive as long as the GObject has >1 references.
         *
         * Since we cannot know how many more wrapped GObjects are going
         * be marked for garbage collection after the owner is destroyed,
         * always queue a garbage collection when a toggle reference goes
         * down.
         */
        GjsContextPrivate* gjs = GjsContextPrivate::from_current_context();
        if (!gjs->destroying())
            gjs->schedule_gc();
    }
}

void
ObjectInstance::toggle_up(void)
{
    /* We need to root the JSObject associated with the passed in GObject so it
     * doesn't get garbage collected (and lose any associated javascript state
     * such as custom properties).
     */
    if (!has_wrapper()) /* Object already GC'd */
        return;

    debug_lifecycle("Toggle notify UP");

    /* Change to strong ref so the wrappee keeps the wrapper alive
     * in case the wrapper has data in it that the app cares about
     */
    if (!wrapper_is_rooted()) {
        /* FIXME: thread the context through somehow. Maybe by looking up
         * the compartment that obj belongs to. */
        GjsContext *context = gjs_context_get_current();
        debug_lifecycle("Rooting wrapper");
        auto cx = static_cast<JSContext *>(gjs_context_get_native_context(context));
        switch_to_rooted(cx);
    }
}

static void
toggle_handler(GObject               *gobj,
               ToggleQueue::Direction direction)
{
    switch (direction) {
        case ToggleQueue::UP:
            ObjectInstance::for_gobject(gobj)->toggle_up();
            break;
        case ToggleQueue::DOWN:
            ObjectInstance::for_gobject(gobj)->toggle_down();
            break;
        default:
            g_assert_not_reached();
    }
}

static void wrapped_gobj_toggle_notify(void*, GObject* gobj,
                                       gboolean is_last_ref) {
    bool is_main_thread;
    bool toggle_up_queued, toggle_down_queued;

    GjsContextPrivate* gjs = GjsContextPrivate::from_current_context();
    if (gjs->destroying()) {
        /* Do nothing here - we're in the process of disassociating
         * the objects.
         */
        return;
    }

    /* We only want to touch javascript from one thread.
     * If we're not in that thread, then we need to defer processing
     * to it.
     * In case we're toggling up (and thus rooting the JS object) we
     * also need to take care if GC is running. The marking side
     * of it is taken care by JS::Heap, which we use in GjsMaybeOwned,
     * so we're safe. As for sweeping, it is too late: the JS object
     * is dead, and attempting to keep it alive would soon crash
     * the process. Plus, if we touch the JSAPI, libmozjs aborts in
     * the first BeginRequest.
     * Thus, we drain the toggle queue when GC starts, in order to
     * prevent this from happening.
     * In practice, a toggle up during JS finalize can only happen
     * for temporary refs/unrefs of objects that are garbage anyway,
     * because JS code is never invoked while the finalizers run
     * and C code needs to clean after itself before it returns
     * from dispose()/finalize().
     * On the other hand, toggling down is a lot simpler, because
     * we're creating more garbage. So we just unroot the object, make it a
     * weak pointer, and wait for the next GC cycle.
     *
     * Note that one would think that toggling up only happens
     * in the main thread (because toggling up is the result of
     * the JS object, previously visible only to JS code, becoming
     * visible to the refcounted C world), but because of weird
     * weak singletons like g_bus_get_sync() objects can see toggle-ups
     * from different threads too.
     */
    is_main_thread = gjs->is_owner_thread();

    auto& toggle_queue = ToggleQueue::get_default();
    std::tie(toggle_down_queued, toggle_up_queued) = toggle_queue.is_queued(gobj);

    if (is_last_ref) {
        /* We've transitions from 2 -> 1 references,
         * The JSObject is rooted and we need to unroot it so it
         * can be garbage collected
         */
        if (is_main_thread) {
            if (G_UNLIKELY (toggle_up_queued || toggle_down_queued)) {
                g_error("toggling down object %s that's already queued to toggle %s\n",
                        G_OBJECT_TYPE_NAME(gobj),
                        toggle_up_queued && toggle_down_queued? "up and down" :
                        toggle_up_queued? "up" : "down");
            }

            ObjectInstance::for_gobject(gobj)->toggle_down();
        } else {
            toggle_queue.enqueue(gobj, ToggleQueue::DOWN, toggle_handler);
        }
    } else {
        /* We've transitioned from 1 -> 2 references.
         *
         * The JSObject associated with the gobject is not rooted,
         * but it needs to be. We'll root it.
         */
        if (is_main_thread && !toggle_down_queued) {
            if (G_UNLIKELY (toggle_up_queued)) {
                g_error("toggling up object %s that's already queued to toggle up\n",
                        G_OBJECT_TYPE_NAME(gobj));
            }
            ObjectInstance::for_gobject(gobj)->toggle_up();
        } else {
            toggle_queue.enqueue(gobj, ToggleQueue::UP, toggle_handler);
        }
    }
}

void
ObjectInstance::release_native_object(void)
{
    discard_wrapper();
    if (m_uses_toggle_ref)
        g_object_remove_toggle_ref(m_ptr, wrapped_gobj_toggle_notify, nullptr);
    else
        g_object_unref(m_ptr);
    m_ptr = nullptr;
}

/* At shutdown, we need to ensure we've cleared the context of any
 * pending toggle references.
 */
void
gjs_object_clear_toggles(void)
{
    auto& toggle_queue = ToggleQueue::get_default();
    while (toggle_queue.handle_toggle(toggle_handler))
        ;
}

void
gjs_object_shutdown_toggle_queue(void)
{
    auto& toggle_queue = ToggleQueue::get_default();
    toggle_queue.shutdown();
}

/*
 * ObjectInstance::prepare_shutdown:
 *
 * Called when the #GjsContext is disposed, in order to release all GC roots of
 * JSObjects that are held by GObjects.
 */
void ObjectInstance::prepare_shutdown(void) {
    /* We iterate over all of the objects, breaking the JS <-> C
     * association.  We avoid the potential recursion implied in:
     *   toggle ref removal -> gobj dispose -> toggle ref notify
     * by emptying the toggle queue earlier in the shutdown sequence. */
    ObjectInstance::remove_wrapped_gobjects_if(
        std::mem_fn(&ObjectInstance::wrapper_is_rooted),
        std::mem_fn(&ObjectInstance::release_native_object));
}

ObjectInstance::ObjectInstance(JSContext* cx, JS::HandleObject object)
    : GIWrapperInstance(cx, object) {
    GJS_INC_COUNTER(object_instance);
}

ObjectPrototype::ObjectPrototype(GIObjectInfo* info, GType gtype)
    : GIWrapperPrototype(info, gtype) {
    g_type_class_ref(gtype);

    GJS_INC_COUNTER(object_prototype);
}

bool ObjectPrototype::init(JSContext* cx) {
    if (!m_property_cache.init() || !m_field_cache.init()) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

/*
 * ObjectInstance::update_heap_wrapper_weak_pointers:
 *
 * Private callback, called after the JS engine finishes garbage collection, and
 * notifies when weak pointers need to be either moved or swept.
 */
void ObjectInstance::update_heap_wrapper_weak_pointers(JSContext*,
                                                       JSCompartment*, void*) {
    gjs_debug_lifecycle(GJS_DEBUG_GOBJECT, "Weak pointer update callback, "
                        "%zu wrapped GObject(s) to examine",
                        ObjectInstance::num_wrapped_gobjects());

    ObjectInstance::remove_wrapped_gobjects_if(
        std::mem_fn(&ObjectInstance::weak_pointer_was_finalized),
        std::mem_fn(&ObjectInstance::disassociate_js_gobject));
}

bool
ObjectInstance::weak_pointer_was_finalized(void)
{
    if (has_wrapper() && !wrapper_is_rooted() && update_after_gc()) {
        /* Ouch, the JS object is dead already. Disassociate the
         * GObject and hope the GObject dies too. (Remove it from
         * the weak pointer list first, since the disassociation
         * may also cause it to be erased.)
         */
        debug_lifecycle("Found GObject weak pointer whose JS wrapper is about "
                        "to be finalized");
        return true;
    }
    return false;
}

/*
 * ObjectInstance::ensure_weak_pointer_callback:
 *
 * Private method called when adding a weak pointer for the first time.
 */
void ObjectInstance::ensure_weak_pointer_callback(JSContext* cx) {
    if (!s_weak_pointer_callback) {
        JS_AddWeakPointerCompartmentCallback(
            cx, &ObjectInstance::update_heap_wrapper_weak_pointers, nullptr);
        s_weak_pointer_callback = true;
    }
}

void
ObjectInstance::associate_js_gobject(JSContext       *context,
                                     JS::HandleObject object,
                                     GObject         *gobj)
{
    g_assert(!wrapper_is_rooted());

    m_uses_toggle_ref = false;
    m_ptr = gobj;
    set_object_qdata();
    m_wrapper = object;

    ensure_weak_pointer_callback(context);
    link();

    g_object_weak_ref(gobj, wrapped_gobj_dispose_notify, this);
}

void
ObjectInstance::ensure_uses_toggle_ref(JSContext *cx)
{
    if (m_uses_toggle_ref)
        return;

    debug_lifecycle("Switching object instance to toggle ref");

    g_assert(!wrapper_is_rooted());

    /* OK, here is where things get complicated. We want the
     * wrapped gobj to keep the JSObject* wrapper alive, because
     * people might set properties on the JSObject* that they care
     * about. Therefore, whenever the refcount on the wrapped gobj
     * is >1, i.e. whenever something other than the wrapper is
     * referencing the wrapped gobj, the wrapped gobj has a strong
     * ref (gc-roots the wrapper). When the refcount on the
     * wrapped gobj is 1, then we change to a weak ref to allow
     * the wrapper to be garbage collected (and thus unref the
     * wrappee).
     */
    m_uses_toggle_ref = true;
    switch_to_rooted(cx);
    g_object_add_toggle_ref(m_ptr, wrapped_gobj_toggle_notify, nullptr);

    /* We now have both a ref and a toggle ref, we only want the toggle ref.
     * This may immediately remove the GC root we just added, since refcount
     * may drop to 1. */
    g_object_unref(m_ptr);
}

void ObjectBase::invalidate_all_closures(void) {
    /* Can't loop directly through the items, since invalidating an item's
     * closure might have the effect of removing the item from the set in the
     * invalidate notifier */
    while (!m_closures.empty()) {
        /* This will also free cd, through the closure invalidation mechanism */
        GClosure *closure = *m_closures.begin();
        g_closure_invalidate(closure);
        /* Erase element if not already erased */
        m_closures.remove(closure);
    }
}

void
ObjectInstance::disassociate_js_gobject(void)
{
    bool had_toggle_down, had_toggle_up;

    if (!m_gobj_disposed)
        g_object_weak_unref(m_ptr, wrapped_gobj_dispose_notify, this);

    auto& toggle_queue = ToggleQueue::get_default();
    std::tie(had_toggle_down, had_toggle_up) = toggle_queue.cancel(m_ptr);
    if (had_toggle_down != had_toggle_up) {
        g_error(
            "JS object wrapper for GObject %p (%s) is being released while "
            "toggle references are still pending.",
            m_ptr, type_name());
    }

    /* Fist, remove the wrapper pointer from the wrapped GObject */
    unset_object_qdata();

    /* Now release all the resources the current wrapper has */
    invalidate_all_closures();
    release_native_object();

    /* Mark that a JS object once existed, but it doesn't any more */
    m_wrapper_finalized = true;
    m_wrapper = nullptr;
}

bool
ObjectInstance::init_impl(JSContext              *context,
                          const JS::CallArgs&     args,
                          JS::MutableHandleObject object)
{
    GTypeQuery query;

    g_assert(gtype() != G_TYPE_NONE);

    std::vector<const char *> names;
    AutoGValueVector values;
    if (!m_proto->props_to_g_parameters(context, args, &names, &values))
        return false;

    if (G_TYPE_IS_ABSTRACT(gtype())) {
        gjs_throw(context,
                  "Cannot instantiate abstract type %s", g_type_name(gtype()));
        return false;
    }

    /* Mark this object in the construction stack, it
       will be popped in gjs_object_custom_init() later
       down.
    */
    if (g_type_get_qdata(gtype(), ObjectInstance::custom_type_quark())) {
        GjsContextPrivate* gjs = GjsContextPrivate::from_cx(context);
        if (!gjs->object_init_list().append(object)) {
            JS_ReportOutOfMemory(context);
            return false;
        }
    }

    g_assert(names.size() == values.size());
    GObject* gobj = g_object_new_with_properties(gtype(), values.size(),
                                                 names.data(), values.data());

    ObjectInstance *other_priv = ObjectInstance::for_gobject(gobj);
    if (other_priv && other_priv->m_wrapper != object.get()) {
        /* g_object_new_with_properties() returned an object that's already
         * tracked by a JS object. Let's assume this is a singleton like
         * IBus.IBus and return the existing JS wrapper object.
         *
         * 'object' has a value that was originally created by
         * JS_NewObjectForConstructor in GJS_NATIVE_CONSTRUCTOR_PRELUDE, but
         * we're not actually using it, so just let it get collected. Avoiding
         * this would require a non-trivial amount of work.
         * */
        other_priv->ensure_uses_toggle_ref(context);
        object.set(other_priv->m_wrapper);
        g_object_unref(gobj); /* We already own a reference */
        gobj = NULL;
        return true;
    }

    type_query_dynamic_safe(&query);
    if (G_LIKELY (query.type))
        JS_updateMallocCounter(context, query.instance_size);

    if (G_IS_INITIALLY_UNOWNED(gobj) &&
        !g_object_is_floating(gobj)) {
        /* GtkWindow does not return a ref to caller of g_object_new.
         * Need a flag in gobject-introspection to tell us this.
         */
        gjs_debug(GJS_DEBUG_GOBJECT,
                  "Newly-created object is initially unowned but we did not get the "
                  "floating ref, probably GtkWindow, using hacky workaround");
        g_object_ref(gobj);
    } else if (g_object_is_floating(gobj)) {
        g_object_ref_sink(gobj);
    } else {
        /* we should already have a ref */
    }

    if (!m_ptr)
        associate_js_gobject(context, object, gobj);

    debug_lifecycle("JSObject created");

    TRACE(GJS_OBJECT_WRAPPER_NEW(this, m_ptr, ns(), name()));

    args.rval().setObject(*object);
    return true;
}

// See GIWrapperBase::constructor()
bool ObjectInstance::constructor_impl(JSContext* context,
                                      JS::HandleObject object,
                                      const JS::CallArgs& argv) {
    JS::RootedValue initer(context);
    GjsContextPrivate* gjs = GjsContextPrivate::from_cx(context);
    return gjs_object_require_property(context, object, "GObject instance",
                                       gjs->atoms().init(), &initer) &&
           gjs->call_function(object, initer, argv, argv.rval());
}

void ObjectBase::trace_impl(JSTracer* tracer) {
    for (GClosure *closure : m_closures)
        gjs_closure_trace(closure, tracer);
}

void ObjectPrototype::trace_impl(JSTracer* tracer) {
    m_property_cache.trace(tracer);
    m_field_cache.trace(tracer);
}

ObjectInstance::~ObjectInstance() {
    TRACE(GJS_OBJECT_WRAPPER_FINALIZE(this, m_ptr, ns(), name()));

    invalidate_all_closures();

    /* GObject is not already freed */
    if (m_ptr) {
        bool had_toggle_up;
        bool had_toggle_down;

        if (G_UNLIKELY(m_ptr->ref_count <= 0)) {
            g_error(
                "Finalizing wrapper for an already freed object of type: "
                "%s.%s\n",
                ns(), name());
        }

        auto& toggle_queue = ToggleQueue::get_default();
        std::tie(had_toggle_down, had_toggle_up) = toggle_queue.cancel(m_ptr);

        if (!had_toggle_up && had_toggle_down) {
            g_error(
                "Finalizing wrapper for an object that's scheduled to be "
                "unrooted: %s.%s\n",
                ns(), name());
        }

        if (!m_gobj_disposed)
            g_object_weak_unref(m_ptr, wrapped_gobj_dispose_notify, this);
        release_native_object();
    }

    if (wrapper_is_rooted()) {
        /* This happens when the refcount on the object is still >1,
         * for example with global objects GDK never frees like GdkDisplay,
         * when we close down the JS runtime.
         */
        gjs_debug(GJS_DEBUG_GOBJECT,
                  "Wrapper was finalized despite being kept alive, has refcount >1");

        debug_lifecycle("Unrooting object");

        discard_wrapper();
    }
    unlink();

    GJS_DEC_COUNTER(object_instance);
}

ObjectPrototype::~ObjectPrototype() {
    invalidate_all_closures();

    g_clear_pointer(&m_info, g_base_info_unref);
    g_type_class_unref(g_type_class_peek(m_gtype));

    GJS_DEC_COUNTER(object_prototype);
}

JSObject* gjs_lookup_object_constructor_from_info(JSContext* context,
                                                  GIObjectInfo* info,
                                                  GType gtype) {
    JS::RootedObject in_object(context);
    const char *constructor_name;

    if (info) {
        in_object = gjs_lookup_namespace_object(context, (GIBaseInfo*) info);
        constructor_name = g_base_info_get_name((GIBaseInfo*) info);
    } else {
        in_object = gjs_lookup_private_namespace(context);
        constructor_name = g_type_name(gtype);
    }

    if (G_UNLIKELY (!in_object))
        return NULL;

    JS::RootedValue value(context);
    if (!JS_GetProperty(context, in_object, constructor_name, &value))
        return NULL;

    JS::RootedObject constructor(context);
    if (value.isUndefined()) {
        /* In case we're looking for a private type, and we don't find it,
           we need to define it first.
        */
        JS::RootedObject ignored(context);
        if (!ObjectPrototype::define_class(context, in_object, nullptr, gtype,
                                           &constructor, &ignored))
            return nullptr;
    } else {
        if (G_UNLIKELY (!value.isObject()))
            return NULL;

        constructor = &value.toObject();
    }

    g_assert(constructor);

    return constructor;
}

GJS_JSAPI_RETURN_CONVENTION
static JSObject *
gjs_lookup_object_prototype_from_info(JSContext    *context,
                                      GIObjectInfo *info,
                                      GType         gtype)
{
    JS::RootedObject constructor(context,
        gjs_lookup_object_constructor_from_info(context, info, gtype));

    if (G_UNLIKELY(!constructor))
        return NULL;

    const GjsAtoms& atoms = GjsContextPrivate::atoms(context);
    JS::RootedObject prototype(context);
    if (!gjs_object_require_property(context, constructor, "constructor object",
                                     atoms.prototype(), &prototype))
        return NULL;

    return prototype;
}

GJS_JSAPI_RETURN_CONVENTION
static JSObject *
gjs_lookup_object_prototype(JSContext *context,
                            GType      gtype)
{
    GjsAutoObjectInfo info = g_irepository_find_by_gtype(nullptr, gtype);
    return gjs_lookup_object_prototype_from_info(context, info, gtype);
}

// Retrieves a GIFieldInfo for a field named @key. This is for use in
// field_getter_impl() and field_setter_not_impl(), where the field info *must*
// have been cached previously in resolve_impl() on this ObjectPrototype or one
// of its parent ObjectPrototypes. This will fail an assertion if there is no
// cached field info.
//
// The caller does not own the return value, and it can never be null.
GIFieldInfo* ObjectPrototype::lookup_cached_field_info(JSContext* cx,
                                                       JS::HandleString key) {
    if (is_custom_js_class()) {
        // Custom JS classes can't have fields. We must be looking up a field on
        // a GObject-introspected parent.
        GType parent_gtype = g_type_parent(m_gtype);
        g_assert(parent_gtype != G_TYPE_INVALID &&
                 "Custom JS class must have parent");
        ObjectPrototype* parent_proto =
            ObjectPrototype::for_gtype(parent_gtype);
        g_assert(parent_proto &&
                 "Custom JS class's parent must have been accessed in JS");
        return parent_proto->lookup_cached_field_info(cx, key);
    }

    gjs_debug_jsprop(GJS_DEBUG_GOBJECT,
                     "Looking up cached field info for '%s' in '%s' prototype",
                     gjs_debug_string(key).c_str(), g_type_name(m_gtype));
    auto entry = m_field_cache.lookupForAdd(key);
    if (entry)
        return entry->value().get();

    // We must be looking up a field defined on a parent. Look up the prototype
    // object via its GIObjectInfo.
    GjsAutoObjectInfo parent_info = g_object_info_get_parent(m_info);
    JS::RootedObject parent_proto(cx, gjs_lookup_object_prototype_from_info(
                                          cx, parent_info, G_TYPE_INVALID));
    ObjectPrototype* parent = ObjectPrototype::for_js(cx, parent_proto);
    return parent->lookup_cached_field_info(cx, key);
}

void ObjectBase::associate_closure(JSContext* cx, GClosure* closure) {
    if (!is_prototype())
        to_instance()->ensure_uses_toggle_ref(cx);

    /* This is a weak reference, and will be cleared when the closure is
     * invalidated */
    auto already_has = std::find(m_closures.begin(), m_closures.end(), closure);
    g_assert(already_has == m_closures.end() &&
             "This closure was already associated with this object");
    m_closures.push_front(closure);
    g_closure_add_invalidate_notifier(closure, this,
                                      &ObjectBase::closure_invalidated_notify);
}

void ObjectBase::closure_invalidated_notify(void* data, GClosure* closure) {
    auto* priv = static_cast<ObjectBase*>(data);
    priv->m_closures.remove(closure);
}

bool ObjectBase::connect(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);
    if (!priv->check_is_instance(cx, "connect to signals"))
        return false;

    return priv->to_instance()->connect_impl(cx, args, false);
}

bool ObjectBase::connect_after(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);
    if (!priv->check_is_instance(cx, "connect to signals"))
        return false;

    return priv->to_instance()->connect_impl(cx, args, true);
}

bool
ObjectInstance::connect_impl(JSContext          *context,
                             const JS::CallArgs& args,
                             bool                after)
{
    GClosure *closure;
    gulong id;
    guint signal_id;
    GQuark signal_detail;

    gjs_debug_gsignal("connect obj %p priv %p", m_wrapper.get(), this);

    if (!check_gobject_disposed("connect to any signal on"))
        return true;

    JS::UniqueChars signal_name;
    JS::RootedObject callback(context);
    if (!gjs_parse_call_args(context, after ? "connect_after" : "connect", args, "so",
                             "signal name", &signal_name,
                             "callback", &callback))
        return false;

    if (!JS::IsCallable(callback)) {
        gjs_throw(context, "second arg must be a callback");
        return false;
    }

    if (!g_signal_parse_name(signal_name.get(), gtype(), &signal_id,
                             &signal_detail, true)) {
        gjs_throw(context, "No signal '%s' on object '%s'",
                  signal_name.get(), type_name());
        return false;
    }

    closure = gjs_closure_new_for_signal(
        context, JS_GetObjectFunction(callback), "signal callback", signal_id);
    if (closure == NULL)
        return false;
    associate_closure(context, closure);

    id = g_signal_connect_closure_by_id(m_ptr, signal_id, signal_detail,
                                        closure, after);

    args.rval().setDouble(id);

    return true;
}

bool ObjectBase::emit(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);
    if (!priv->check_is_instance(cx, "emit signal"))
        return false;

    return priv->to_instance()->emit_impl(cx, args);
}

bool
ObjectInstance::emit_impl(JSContext          *context,
                          const JS::CallArgs& argv)
{
    guint signal_id;
    GQuark signal_detail;
    GSignalQuery signal_query;
    GValue *instance_and_args;
    GValue rvalue = G_VALUE_INIT;
    unsigned int i;
    bool failed;

    gjs_debug_gsignal("emit obj %p priv %p argc %d", m_wrapper.get(), this,
                      argv.length());

    if (!check_gobject_disposed("emit any signal on"))
        return true;

    JS::UniqueChars signal_name;
    if (!gjs_parse_call_args(context, "emit", argv, "!s",
                             "signal name", &signal_name))
        return false;

    if (!g_signal_parse_name(signal_name.get(), gtype(), &signal_id,
                             &signal_detail, false)) {
        gjs_throw(context, "No signal '%s' on object '%s'",
                  signal_name.get(), type_name());
        return false;
    }

    g_signal_query(signal_id, &signal_query);

    if ((argv.length() - 1) != signal_query.n_params) {
        gjs_throw(context, "Signal '%s' on %s requires %d args got %d",
                  signal_name.get(), type_name(), signal_query.n_params,
                  argv.length() - 1);
        return false;
    }

    if (signal_query.return_type != G_TYPE_NONE) {
        g_value_init(&rvalue, signal_query.return_type & ~G_SIGNAL_TYPE_STATIC_SCOPE);
    }

    instance_and_args = g_newa(GValue, signal_query.n_params + 1);
    memset(instance_and_args, 0, sizeof(GValue) * (signal_query.n_params + 1));

    g_value_init(&instance_and_args[0], gtype());
    g_value_set_instance(&instance_and_args[0], m_ptr);

    failed = false;
    for (i = 0; i < signal_query.n_params; ++i) {
        GValue *value;
        value = &instance_and_args[i + 1];

        g_value_init(value, signal_query.param_types[i] & ~G_SIGNAL_TYPE_STATIC_SCOPE);
        if ((signal_query.param_types[i] & G_SIGNAL_TYPE_STATIC_SCOPE) != 0)
            failed = !gjs_value_to_g_value_no_copy(context, argv[i + 1], value);
        else
            failed = !gjs_value_to_g_value(context, argv[i + 1], value);

        if (failed)
            break;
    }

    if (!failed) {
        g_signal_emitv(instance_and_args, signal_id, signal_detail,
                       &rvalue);
    }

    if (signal_query.return_type != G_TYPE_NONE) {
        if (!gjs_value_from_g_value(context, argv.rval(), &rvalue))
            failed = true;

        g_value_unset(&rvalue);
    } else {
        argv.rval().setUndefined();
    }

    for (i = 0; i < (signal_query.n_params + 1); ++i) {
        g_value_unset(&instance_and_args[i]);
    }

    return !failed;
}

bool ObjectBase::to_string(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, obj, ObjectBase, priv);
    return gjs_wrapper_to_string_func(
        cx, obj, priv->to_string_kind(), priv->info(), priv->gtype(),
        priv->is_prototype() ? nullptr : priv->to_instance()->ptr(),
        args.rval());
}

// Override of GIWrapperBase::to_string_kind()
const char* ObjectBase::to_string_kind(void) const {
    if (is_prototype())
        return "object";
    return to_instance()->to_string_kind();
}

/*
 * ObjectInstance::to_string_kind:
 *
 * Instance-only version of GIWrapperBase::to_string_kind(). ObjectInstance
 * shows a "finalized" marker in its toString() method if the wrapped GObject
 * has already been finalized.
 */
const char* ObjectInstance::to_string_kind(void) const {
    return m_gobj_disposed ? "object (FINALIZED)" : "object";
}

/*
 * ObjectBase::init_gobject:
 *
 * This is named "init_gobject()" but corresponds to "_init()" in JS. The reason
 * for the name is that an "init()" method is used within SpiderMonkey to
 * indicate fallible initialization that must be done before an object can be
 * used, which is not the case here.
 */
bool ObjectBase::init_gobject(JSContext* context, unsigned argc,
                              JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(context, argc, vp, argv, obj, ObjectBase, priv);
    if (!priv->check_is_instance(context, "initialize"))
        return false;

    return priv->to_instance()->init_impl(context, argv, &obj);
}

// clang-format off
const struct JSClassOps ObjectBase::class_ops = {
    &ObjectBase::add_property,
    nullptr,  // deleteProperty
    nullptr,  // enumerate
    &ObjectBase::new_enumerate,
    &ObjectBase::resolve,
    nullptr,  // mayResolve
    &ObjectBase::finalize,
    NULL,
    NULL,
    NULL,
    &ObjectBase::trace,
};

const struct JSClass ObjectBase::klass = {
    "GObject_Object",
    JSCLASS_HAS_PRIVATE | JSCLASS_FOREGROUND_FINALIZE,
    &ObjectBase::class_ops
};

JSFunctionSpec ObjectBase::proto_methods[] = {
    JS_FN("_init", &ObjectBase::init_gobject, 0, 0),
    JS_FN("connect", &ObjectBase::connect, 0, 0),
    JS_FN("connect_after", &ObjectBase::connect_after, 0, 0),
    JS_FN("emit", &ObjectBase::emit, 0, 0),
    JS_FS_END
};
// clang-format on

// Override of GIWrapperPrototype::get_parent_proto()
bool ObjectPrototype::get_parent_proto(JSContext* cx,
                                       JS::MutableHandleObject proto) const {
    GType parent_type = g_type_parent(gtype());
    if (parent_type != G_TYPE_INVALID) {
        proto.set(gjs_lookup_object_prototype(cx, parent_type));
        if (!proto)
            return false;
    }
    return true;
}

/*
 * ObjectPrototype::define_class:
 * @in_object: Object where the constructor is stored, typically a repo object.
 * @info: Introspection info for the GObject class.
 * @gtype: #GType for the GObject class.
 * @constructor: Return location for the constructor object.
 * @prototype: Return location for the prototype object.
 *
 * Define a GObject class constructor and prototype, including all the
 * necessary methods and properties that are not introspected. Provides the
 * constructor and prototype objects as out parameters, for convenience
 * elsewhere.
 */
bool ObjectPrototype::define_class(JSContext* context,
                                   JS::HandleObject in_object,
                                   GIObjectInfo* info, GType gtype,
                                   JS::MutableHandleObject constructor,
                                   JS::MutableHandleObject prototype) {
    if (!ObjectPrototype::create_class(context, in_object, info, gtype,
                                       constructor, prototype))
        return false;

    /* Hook_up_vfunc can't be included in gjs_object_instance_proto_funcs
     * because it's a custom symbol. */
    const GjsAtoms& atoms = GjsContextPrivate::atoms(context);
    return JS_DefineFunctionById(context, prototype, atoms.hook_up_vfunc(),
                                 &ObjectBase::hook_up_vfunc, 3,
                                 GJS_MODULE_PROP_FLAGS);
}

/*
 * ObjectInstance::init_custom_class_from_gobject:
 *
 * Does all the necessary initialization for an ObjectInstance and JSObject
 * wrapper, given a newly-created GObject pointer, of a GObject class that was
 * created in JS with GObject.registerClass(). This is called from the GObject's
 * instance init function in gobject.cpp, and that's the only reason it's a
 * public method.
 */
bool ObjectInstance::init_custom_class_from_gobject(JSContext* cx,
                                                    JS::HandleObject wrapper,
                                                    GObject* gobj) {
    associate_js_gobject(cx, wrapper, gobj);

    // Custom JS objects will most likely have visible state, so just do this
    // from the start.
    ensure_uses_toggle_ref(cx);

    const GjsAtoms& atoms = GjsContextPrivate::atoms(cx);
    JS::RootedValue v(cx);
    if (!JS_GetPropertyById(cx, wrapper, atoms.instance_init(), &v))
        return false;

    if (v.isUndefined())
        return true;
    if (!v.isObject() || !JS::IsCallable(&v.toObject())) {
        gjs_throw(cx, "_instance_init property was not a function");
        return false;
    }

    JS::RootedValue ignored_rval(cx);
    return JS_CallFunctionValue(cx, wrapper, v, JS::HandleValueArray::empty(),
                                &ignored_rval);
}

/*
 * ObjectInstance::new_for_gobject:
 *
 * Creates a new JSObject wrapper for the GObject pointer @gobj, and an
 * ObjectInstance private structure to go along with it.
 */
ObjectInstance* ObjectInstance::new_for_gobject(JSContext* cx, GObject* gobj) {
    g_assert(gobj && "Cannot create JSObject for null GObject pointer");

    GType gtype = G_TYPE_FROM_INSTANCE(gobj);

    gjs_debug_marshal(GJS_DEBUG_GOBJECT, "Wrapping %s with JSObject",
                      g_type_name(gtype));

    JS::RootedObject proto(cx, gjs_lookup_object_prototype(cx, gtype));
    if (!proto)
        return nullptr;

    JS::RootedObject obj(
        cx, JS_NewObjectWithGivenProto(cx, JS_GetClass(proto), proto));
    if (!obj)
        return nullptr;

    ObjectInstance* priv = ObjectInstance::new_for_js_object(cx, obj);

    g_object_ref_sink(gobj);
    priv->associate_js_gobject(cx, obj, gobj);

    g_assert(priv->wrapper() == obj.get());

    return priv;
}

/*
 * ObjectInstance::wrapper_from_gobject:
 *
 * Gets a JSObject wrapper for the GObject pointer @gobj. If one already exists,
 * then it is returned. Otherwise a new one is created with
 * ObjectInstance::new_for_gobject().
 */
JSObject* ObjectInstance::wrapper_from_gobject(JSContext* cx, GObject* gobj) {
    g_assert(gobj && "Cannot get JSObject for null GObject pointer");

    ObjectInstance* priv = ObjectInstance::for_gobject(gobj);

    if (!priv) {
        /* We have to create a wrapper */
        priv = new_for_gobject(cx, gobj);
        if (!priv)
            return nullptr;
    }

    return priv->wrapper();
}

// Replaces GIWrapperBase::to_c_ptr(). The GIWrapperBase version is deleted.
bool ObjectBase::to_c_ptr(JSContext* cx, JS::HandleObject obj, GObject** ptr) {
    g_assert(ptr);

    auto* priv = ObjectBase::for_js(cx, obj);
    if (!priv || priv->is_prototype())
        return false;

    ObjectInstance* instance = priv->to_instance();
    if (!instance->check_gobject_disposed("access")) {
        *ptr = nullptr;
        return true;
    }

    *ptr = instance->ptr();
    return true;
}

// Overrides GIWrapperBase::transfer_to_gi_argument().
bool ObjectBase::transfer_to_gi_argument(JSContext* cx, JS::HandleObject obj,
                                         GIArgument* arg,
                                         GIDirection transfer_direction,
                                         GITransfer transfer_ownership,
                                         GType expected_gtype,
                                         GIBaseInfo* expected_info) {
    g_assert(transfer_direction != GI_DIRECTION_INOUT &&
             "transfer_to_gi_argument() must choose between in or out");

    if (!ObjectBase::typecheck(cx, obj, expected_info, expected_gtype)) {
        arg->v_pointer = nullptr;
        return false;
    }

    GObject* ptr;
    if (!ObjectBase::to_c_ptr(cx, obj, &ptr))
        return false;

    // Pointer can be null if object was already disposed by C code
    if (!ptr) {
        arg->v_pointer = nullptr;
        return true;
    }

    arg->v_pointer = ptr;

    if ((transfer_direction == GI_DIRECTION_IN &&
         transfer_ownership != GI_TRANSFER_NOTHING) ||
        (transfer_direction == GI_DIRECTION_OUT &&
         transfer_ownership == GI_TRANSFER_EVERYTHING)) {
        arg->v_pointer =
            ObjectInstance::copy_ptr(cx, expected_gtype, arg->v_pointer);
        if (!arg->v_pointer)
            return false;
    }

    return true;
}

// Overrides GIWrapperInstance::typecheck_impl()
bool ObjectInstance::typecheck_impl(JSContext* cx, GIBaseInfo* expected_info,
                                    GType expected_type) const {
    g_assert(m_gobj_disposed || gtype() == G_OBJECT_TYPE(m_ptr));
    return GIWrapperInstance::typecheck_impl(cx, expected_info, expected_type);
}

GJS_JSAPI_RETURN_CONVENTION
static bool find_vfunc_info(JSContext* context, GType implementor_gtype,
                            GIBaseInfo* vfunc_info, const char* vfunc_name,
                            void** implementor_vtable_ret,
                            GjsAutoFieldInfo* field_info_ret) {
    GType ancestor_gtype;
    int length, i;
    GIBaseInfo *ancestor_info;
    GjsAutoStructInfo struct_info;
    bool is_interface;

    field_info_ret->reset();
    *implementor_vtable_ret = NULL;

    ancestor_info = g_base_info_get_container(vfunc_info);
    ancestor_gtype = g_registered_type_info_get_g_type((GIRegisteredTypeInfo*)ancestor_info);

    is_interface = g_base_info_get_type(ancestor_info) == GI_INFO_TYPE_INTERFACE;

    GjsAutoTypeClass<GTypeClass> implementor_class(implementor_gtype);
    if (is_interface) {
        GTypeInstance *implementor_iface_class;
        implementor_iface_class = (GTypeInstance*) g_type_interface_peek(implementor_class,
                                                        ancestor_gtype);
        if (implementor_iface_class == NULL) {
            gjs_throw (context, "Couldn't find GType of implementor of interface %s.",
                       g_type_name(ancestor_gtype));
            return false;
        }

        *implementor_vtable_ret = implementor_iface_class;

        struct_info = g_interface_info_get_iface_struct((GIInterfaceInfo*)ancestor_info);
    } else {
        struct_info = g_object_info_get_class_struct((GIObjectInfo*)ancestor_info);
        *implementor_vtable_ret = implementor_class;
    }

    length = g_struct_info_get_n_fields(struct_info);
    for (i = 0; i < length; i++) {
        GjsAutoFieldInfo field_info = g_struct_info_get_field(struct_info, i);
        if (strcmp(field_info.name(), vfunc_name) != 0)
            continue;

        GjsAutoTypeInfo type_info = g_field_info_get_type(field_info);
        if (g_type_info_get_tag(type_info) != GI_TYPE_TAG_INTERFACE) {
            /* We have a field with the same name, but it's not a callback.
             * There's no hope of being another field with a correct name,
             * so just abort early. */
            return true;
        } else {
            *field_info_ret = std::move(field_info);
            return true;
        }
    }
    return true;
}

bool ObjectBase::hook_up_vfunc(JSContext* cx, unsigned argc, JS::Value* vp) {
    GJS_GET_WRAPPER_PRIV(cx, argc, vp, args, prototype, ObjectBase, priv);
    /* Normally we wouldn't assert is_prototype(), but this method can only be
     * called internally so it's OK to crash if done wrongly */
    return priv->to_prototype()->hook_up_vfunc_impl(cx, args, prototype);
}

bool ObjectPrototype::hook_up_vfunc_impl(JSContext* cx,
                                         const JS::CallArgs& args,
                                         JS::HandleObject prototype) {
    JS::UniqueChars name;
    JS::RootedObject function(cx);
    if (!gjs_parse_call_args(cx, "hook_up_vfunc", args, "so",
                             "name", &name,
                             "function", &function))
        return false;

    args.rval().setUndefined();

    /* find the first class that actually has repository information */
    GIObjectInfo *info = m_info;
    GType info_gtype = m_gtype;
    while (!info && info_gtype != G_TYPE_OBJECT) {
        info_gtype = g_type_parent(info_gtype);

        info = g_irepository_find_by_gtype(nullptr, info_gtype);
    }

    /* If we don't have 'info', we don't have the base class (GObject).
     * This is awful, so abort now. */
    g_assert(info != NULL);

    GjsAutoVFuncInfo vfunc = find_vfunc_on_parents(info, name.get(), nullptr);

    if (!vfunc) {
        guint i, n_interfaces;
        GType *interface_list;

        interface_list = g_type_interfaces(m_gtype, &n_interfaces);

        for (i = 0; i < n_interfaces; i++) {
            GjsAutoInterfaceInfo interface =
                g_irepository_find_by_gtype(nullptr, interface_list[i]);

            /* The interface doesn't have to exist -- it could be private
             * or dynamic. */
            if (interface) {
                vfunc = g_interface_info_find_vfunc(interface, name.get());

                if (vfunc)
                    break;
            }
        }

        g_free(interface_list);
    }

    if (!vfunc) {
        gjs_throw(cx, "Could not find definition of virtual function %s",
                  name.get());
        return false;
    }

    void *implementor_vtable;
    GjsAutoFieldInfo field_info;
    if (!find_vfunc_info(cx, m_gtype, vfunc, name.get(), &implementor_vtable,
                         &field_info))
        return false;

    if (field_info) {
        gint offset;
        gpointer method_ptr;
        GjsCallbackTrampoline *trampoline;

        offset = g_field_info_get_offset(field_info);
        method_ptr = G_STRUCT_MEMBER_P(implementor_vtable, offset);

        if (!JS_ObjectIsFunction(cx, function)) {
            gjs_throw(cx, "Tried to deal with a vfunc that wasn't a function");
            return false;
        }
        JS::RootedFunction func(cx, JS_GetObjectFunction(function));
        trampoline = gjs_callback_trampoline_new(
            cx, func, vfunc, GI_SCOPE_TYPE_NOTIFIED, prototype, true);

        *((ffi_closure **)method_ptr) = trampoline->closure;
    }

    return true;
}

bool
gjs_lookup_object_constructor(JSContext             *context,
                              GType                  gtype,
                              JS::MutableHandleValue value_p)
{
    JSObject *constructor;

    GjsAutoObjectInfo object_info = g_irepository_find_by_gtype(nullptr, gtype);

    constructor = gjs_lookup_object_constructor_from_info(context, object_info, gtype);

    if (G_UNLIKELY (constructor == NULL))
        return false;

    value_p.setObject(*constructor);
    return true;
}
