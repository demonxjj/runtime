/*
 * icall.c:
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Paolo Molaro (lupus@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <mono/metadata/object.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/reflection.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/file-io.h>
#include <mono/metadata/socket-io.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/unicode.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/rand.h>
#include <mono/io-layer/io-layer.h>
#include "decimal.h"


static MonoObject *
ves_icall_System_Array_GetValueImpl (MonoObject *this, guint32 pos)
{
	MonoClass *ac;
	MonoArray *ao;
	gint32 esize;
	gpointer *ea;

	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;

	esize = mono_array_element_size (ac);
	ea = (gpointer*)((char*)ao->vector + (pos * esize));

	if (ac->element_class->valuetype)
		return mono_value_box (this->vtable->domain, ac->element_class, ea);
	else
		return *ea;
}

static MonoObject *
ves_icall_System_Array_GetValue (MonoObject *this, MonoObject *idxs)
{
	MonoClass *ac, *ic;
	MonoArray *ao, *io;
	gint32 i, pos, *ind;

	MONO_CHECK_ARG_NULL (idxs);

	io = (MonoArray *)idxs;
	ic = (MonoClass *)io->obj.vtable->klass;
	
	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;

	g_assert (ic->rank == 1);
	if (io->bounds [0].length != ac->rank)
		mono_raise_exception (mono_get_exception_argument (NULL, NULL));

	ind = (guint32 *)io->vector;
	for (i = 0; i < ac->rank; i++)
		if ((ind [i] < ao->bounds [i].lower_bound) ||
		    (ind [i] >= ao->bounds [i].length + ao->bounds [i].lower_bound))
			mono_raise_exception (mono_get_exception_index_out_of_range ());

	pos = ind [0] - ao->bounds [0].lower_bound;
	for (i = 1; i < ac->rank; i++)
		pos = pos*ao->bounds [i].length + ind [i] - 
			ao->bounds [i].lower_bound;

	return ves_icall_System_Array_GetValueImpl (this, pos);
}

static void
ves_icall_System_Array_SetValueImpl (MonoObject *this, MonoObject *value, guint32 pos)
{
	MonoArray *ao, *vo;
	MonoClass *ac, *vc, *ec;
	gint32 esize, vsize;
	gpointer *ea, *va;

	guint64 u64;
	gint64 i64;
	gdouble r64;

	vo = (MonoArray *)value;
	if (vo)
		vc = (MonoClass *)vo->obj.vtable->klass;
	else
		vc = NULL;

	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;
	ec = ac->element_class;

	esize = mono_array_element_size (ac);
	ea = (gpointer*)((char*)ao->vector + (pos * esize));
	va = (gpointer*)((char*)vo + sizeof (MonoObject));

	if (!vo) {
		memset (ea, 0,  esize);
		return;
	}

#define NO_WIDENING_CONVERSION G_STMT_START{\
	mono_raise_exception (mono_get_exception_argument ( \
		"value", "not a widening conversion")); \
}G_STMT_END

#define CHECK_WIDENING_CONVERSION(extra) G_STMT_START{\
	if (esize < vsize + ## extra) \
		mono_raise_exception (mono_get_exception_argument ( \
			"value", "not a widening conversion")); \
}G_STMT_END

#define INVALID_CAST G_STMT_START{\
	mono_raise_exception (mono_get_exception_invalid_cast ()); \
}G_STMT_END

	/* Check element (destination) type. */
	switch (ec->byval_arg.type) {
	case MONO_TYPE_STRING:
		switch (vc->byval_arg.type) {
		case MONO_TYPE_STRING:
			break;
		default:
			INVALID_CAST;
		}
		break;
	case MONO_TYPE_BOOLEAN:
		switch (vc->byval_arg.type) {
		case MONO_TYPE_BOOLEAN:
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
		case MONO_TYPE_U8:
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
		case MONO_TYPE_I4:
		case MONO_TYPE_I8:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			NO_WIDENING_CONVERSION;
		default:
			INVALID_CAST;
		}
		break;
	}

	if (!ec->valuetype) {
		*ea = (gpointer)vo;
		return;
	}

	if (mono_object_isinst (value, ec)) {
		memcpy (ea, (char *)vo + sizeof (MonoObject), esize);
		return;
	}

	if (!vc->valuetype)
		INVALID_CAST;

	vsize = mono_class_instance_size (vc) - sizeof (MonoObject);

#if 0
	g_message (G_STRLOC ": %d (%d) <= %d (%d)",
		   ec->byval_arg.type, esize,
		   vc->byval_arg.type, vsize);
#endif

#define ASSIGN_UNSIGNED(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		CHECK_WIDENING_CONVERSION(0); \
		*(## etype *) ea = (## etype) u64; \
		return; \
	/* You can't assign a signed value to an unsigned array. */ \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
	/* You can't assign a floating point number to an integer array. */ \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		NO_WIDENING_CONVERSION; \
	} \
}G_STMT_END

#define ASSIGN_SIGNED(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
		CHECK_WIDENING_CONVERSION(0); \
		*(## etype *) ea = (## etype) i64; \
		return; \
	/* You can assign an unsigned value to a signed array if the array's */ \
	/* element size is larger than the value size. */ \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		CHECK_WIDENING_CONVERSION(1); \
		*(## etype *) ea = (## etype) u64; \
		return; \
	/* You can't assign a floating point number to an integer array. */ \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		NO_WIDENING_CONVERSION; \
	} \
}G_STMT_END

#define ASSIGN_REAL(etype) G_STMT_START{\
	switch (vc->byval_arg.type) { \
	case MONO_TYPE_R4: \
	case MONO_TYPE_R8: \
		CHECK_WIDENING_CONVERSION(0); \
		*(## etype *) ea = (## etype) r64; \
		return; \
	/* All integer values fit into a floating point array, so we don't */ \
	/* need to CHECK_WIDENING_CONVERSION here. */ \
	case MONO_TYPE_I1: \
	case MONO_TYPE_I2: \
	case MONO_TYPE_I4: \
	case MONO_TYPE_I8: \
		*(## etype *) ea = (## etype) i64; \
		return; \
	case MONO_TYPE_U1: \
	case MONO_TYPE_U2: \
	case MONO_TYPE_U4: \
	case MONO_TYPE_U8: \
	case MONO_TYPE_CHAR: \
		*(## etype *) ea = (## etype) u64; \
		return; \
	} \
}G_STMT_END

	switch (vc->byval_arg.type) {
	case MONO_TYPE_U1:
		u64 = *(guint8 *) va;
		break;
	case MONO_TYPE_U2:
		u64 = *(guint16 *) va;
		break;
	case MONO_TYPE_U4:
		u64 = *(guint32 *) va;
		break;
	case MONO_TYPE_U8:
		u64 = *(guint64 *) va;
		break;
	case MONO_TYPE_I1:
		i64 = *(gint8 *) va;
		break;
	case MONO_TYPE_I2:
		i64 = *(gint16 *) va;
		break;
	case MONO_TYPE_I4:
		i64 = *(gint32 *) va;
		break;
	case MONO_TYPE_I8:
		i64 = *(gint64 *) va;
		break;
	case MONO_TYPE_R4:
		r64 = *(gfloat *) va;
		break;
	case MONO_TYPE_R8:
		r64 = *(gdouble *) va;
		break;
	case MONO_TYPE_CHAR:
		u64 = *(guint16 *) va;
		break;
	case MONO_TYPE_BOOLEAN:
		/* Boolean is only compatible with itself. */
		switch (ec->byval_arg.type) {
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
		case MONO_TYPE_U8:
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
		case MONO_TYPE_I4:
		case MONO_TYPE_I8:
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			NO_WIDENING_CONVERSION;
		default:
			INVALID_CAST;
		}
		break;
	}

	/* If we can't do a direct copy, let's try a widening conversion. */
	switch (ec->byval_arg.type) {
	case MONO_TYPE_CHAR:
		ASSIGN_UNSIGNED (guint16);
	case MONO_TYPE_U1:
		ASSIGN_UNSIGNED (guint8);
	case MONO_TYPE_U2:
		ASSIGN_UNSIGNED (guint16);
	case MONO_TYPE_U4:
		ASSIGN_UNSIGNED (guint32);
	case MONO_TYPE_U8:
		ASSIGN_UNSIGNED (guint64);
	case MONO_TYPE_I1:
		ASSIGN_SIGNED (gint8);
	case MONO_TYPE_I2:
		ASSIGN_SIGNED (gint16);
	case MONO_TYPE_I4:
		ASSIGN_SIGNED (gint32);
	case MONO_TYPE_I8:
		ASSIGN_SIGNED (gint64);
	case MONO_TYPE_R4:
		ASSIGN_REAL (gfloat);
	case MONO_TYPE_R8:
		ASSIGN_REAL (gdouble);
	}

	INVALID_CAST;
	/* Not reached, INVALID_CAST does not return. Just to avoid a compiler warning ... */
	return;

#undef INVALID_CAST
#undef NO_WIDENING_CONVERSION
#undef CHECK_WIDENING_CONVERSION
#undef ASSIGN_UNSIGNED
#undef ASSIGN_SIGNED
#undef ASSIGN_REAL
}

static void 
ves_icall_System_Array_SetValue (MonoObject *this, MonoObject *value,
				 MonoObject *idxs)
{
	MonoArray *ao, *io, *vo;
	MonoClass *ac, *ic, *vc;
	gint32 i, pos, *ind;

	MONO_CHECK_ARG_NULL (idxs);

	vo = (MonoArray *)value;
	if (vo)
		vc = (MonoClass *)vo->obj.vtable->klass;
	else
		vc = NULL;

	io = (MonoArray *)idxs;
	ic = (MonoClass *)io->obj.vtable->klass;
	
	ao = (MonoArray *)this;
	ac = (MonoClass *)ao->obj.vtable->klass;

	g_assert (ic->rank == 1);
	if (io->bounds [0].length != ac->rank)
		mono_raise_exception (mono_get_exception_argument (NULL, NULL));

	ind = (guint32 *)io->vector;
	for (i = 0; i < ac->rank; i++)
		if ((ind [i] < ao->bounds [i].lower_bound) ||
		    (ind [i] >= ao->bounds [i].length + ao->bounds [i].lower_bound))
			mono_raise_exception (mono_get_exception_index_out_of_range ());

	pos = ind [0] - ao->bounds [0].lower_bound;
	for (i = 1; i < ac->rank; i++)
		pos = pos*ao->bounds [i].length + ind [i] - 
			ao->bounds [i].lower_bound;

	ves_icall_System_Array_SetValueImpl (this, value, pos);
}

static MonoArray *
ves_icall_System_Array_CreateInstanceImpl (MonoReflectionType *type, MonoArray *lengths, MonoArray *bounds)
{
	MonoClass *aklass;
	MonoArray *array;
	gint32 *sizes, i;

	MONO_CHECK_ARG_NULL (type);
	MONO_CHECK_ARG_NULL (lengths);

	MONO_CHECK_ARG (lengths, mono_array_length (lengths) > 0);
	if (bounds)
		MONO_CHECK_ARG (bounds, mono_array_length (lengths) == mono_array_length (bounds));

	for (i = 0; i < mono_array_length (lengths); i++)
		if (mono_array_get (lengths, gint32, i) < 0)
			mono_raise_exception (mono_get_exception_argument_out_of_range (NULL));

	aklass = mono_array_class_get (type->type, mono_array_length (lengths));

	sizes = alloca (aklass->rank * sizeof(guint32) * 2);
	for (i = 0; i < aklass->rank; ++i) {
		sizes [i] = mono_array_get (lengths, gint32, i);
		if (bounds)
			sizes [i + aklass->rank] = mono_array_get (bounds, gint32, i);
		else
			sizes [i + aklass->rank] = 0;
	}

	array = mono_array_new_full (mono_domain_get (), aklass, sizes, sizes + aklass->rank);

	return array;
}

static gint32 
ves_icall_System_Array_GetRank (MonoObject *this)
{
	return this->vtable->klass->rank;
}

static gint32
ves_icall_System_Array_GetLength (MonoArray *this, gint32 dimension)
{
	gint32 rank = ((MonoObject *)this)->vtable->klass->rank;
	if ((dimension < 0) || (dimension >= rank))
		mono_raise_exception (mono_get_exception_index_out_of_range ());
	return this->bounds [dimension].length;
}

static gint32
ves_icall_System_Array_GetLowerBound (MonoArray *this, gint32 dimension)
{
	gint32 rank = ((MonoObject *)this)->vtable->klass->rank;
	if ((dimension < 0) || (dimension >= rank))
		mono_raise_exception (mono_get_exception_index_out_of_range ());
	return this->bounds [dimension].lower_bound;
}

static void
ves_icall_System_Array_FastCopy (MonoArray *source, int source_idx, MonoArray* dest, int dest_idx, int length)
{
	int element_size = mono_array_element_size (source->obj.vtable->klass);
	void * dest_addr = mono_array_addr_with_size (dest, element_size, dest_idx);
	void * source_addr = mono_array_addr_with_size (source, element_size, dest_idx);

	memcpy (dest_addr, source_addr, element_size * length);
}

static void
ves_icall_InitializeArray (MonoArray *array, MonoClassField *field_handle)
{
	MonoClass *klass = array->obj.vtable->klass;
	guint32 size = mono_array_element_size (klass);
	int i;

	for (i = 0; i < klass->rank; ++i) 
		size *= array->bounds [i].length;

	memcpy (mono_array_addr (array, char, 0), field_handle->data, size);

#if G_BYTE_ORDER != G_LITTLE_ENDIAN
#define SWAP(n) {\
	gint i; \
	guint ## n tmp; \
	guint ## n *data = (guint ## n *) mono_array_addr (array, char, 0); \
\
	for (i = 0; i < size; i += n/8, data++) { \
		tmp = read ## n (data); \
		*data = tmp; \
	} \
}

	printf ("Initialize array with elements of %s type\n", klass->element_class->name);

	switch (klass->element_class->byval_arg.type) {
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		SWAP (16);
		break;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		SWAP (32);
		break;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		SWAP (64);
		break;
	}
		 
#endif
}

static MonoObject *
ves_icall_System_Object_MemberwiseClone (MonoObject *this)
{
	return mono_object_clone (this);
}

static MonoReflectionType *
ves_icall_System_Object_GetType (MonoObject *obj)
{
	return mono_type_get_object (mono_domain_get (), &obj->vtable->klass->byval_arg);
}

static void
mono_type_type_from_obj (MonoReflectionType *mtype, MonoObject *obj)
{
	mtype->type = &obj->vtable->klass->byval_arg;
	g_assert (mtype->type->type);
}

static gint32
ves_icall_AssemblyBuilder_getToken (MonoReflectionAssemblyBuilder *assb, MonoObject *obj)
{
	return mono_image_create_token (assb->dynamic_assembly, obj);
}

static gint32
ves_icall_get_data_chunk (MonoReflectionAssemblyBuilder *assb, gint32 type, MonoArray *buf)
{
	int count;

	if (type == 0) { /* get the header */
		count = mono_image_get_header (assb, (char*)buf->vector, buf->bounds->length);
		if (count != -1)
			return count;
	} else {
		MonoDynamicAssembly *ass = assb->dynamic_assembly;
		char *p = mono_array_addr (buf, char, 0);
		count = ass->code.index + ass->meta_size;
		if (count > buf->bounds->length) {
			g_print ("assembly data exceed supplied buffer\n");
			return 0;
		}
		memcpy (p, ass->code.data, ass->code.index);
		memcpy (p + ass->code.index, ass->assembly.image->raw_metadata, ass->meta_size);
		return count;
	}
	
	return 0;
}

static MonoReflectionType*
ves_icall_type_from_name (MonoString *name)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *klass;
	MonoImage *image;
	MonoTypeNameParse info;
	gchar *str;
	
	str = mono_string_to_utf8 (name);
	/*g_print ("requested type %s\n", str);*/
	if (!mono_reflection_parse_type (str, &info)) {
		g_free (str);
		return NULL;
	}

	if (info.assembly) {
		image = mono_image_loaded (info.assembly);
		/* do we need to load if it's not already loaded? */
		if (!image) {
			g_free (str);
			return NULL;
		}
	} else
		image = mono_defaults.corlib;
	if (info.nest_name) {
		klass = mono_class_from_name (image, info.nest_name_space, info.nest_name);
		if (klass) {
			GList *nested;
			mono_class_init (klass);
			nested = klass->nested_classes;
			while (nested) {
				klass = nested->data;
				if (strcmp (klass->name, info.nest_name) == 0 &&
						strcmp (klass->name_space, info.nest_name_space) == 0)
					break;
				klass = NULL;
			}
		}
	} else {
		klass = mono_class_from_name (image, info.name_space, info.name);
	}
	g_free (str);
	if (!klass)
		return NULL;
	mono_class_init (klass);
	if (info.rank)
		klass = mono_array_class_get (&klass->byval_arg, info.rank);
	
	if (info.isbyref || info.ispointer) /* hack */
		return mono_type_get_object (domain, &klass->this_arg);
	else
		return mono_type_get_object (domain, &klass->byval_arg);
}

static MonoReflectionType*
ves_icall_type_from_handle (MonoType *handle)
{
	MonoDomain *domain = mono_domain_get (); 

	mono_class_init (handle->data.klass);
	return mono_type_get_object (domain, handle);
}

static guint32
ves_icall_type_Equals (MonoReflectionType *type, MonoReflectionType *c)
{
	if (type->type && c->type)
		return mono_metadata_type_equal (type->type, c->type);
	g_print ("type equals\n");
	return 0;
}

static guint32
ves_icall_type_is_subtype_of (MonoReflectionType *type, MonoReflectionType *c, MonoBoolean check_interfaces)
{
	MonoDomain *domain; 
	MonoClass *klass;
	MonoClass *klassc;

	g_assert (type != NULL);
	
	domain = ((MonoObject *)type)->vtable->domain;

	if (!c) /* FIXME: dont know what do do here */
		return 0;

	klass = mono_class_from_mono_type (type->type);
	klassc = mono_class_from_mono_type (c->type);

	/* cut&paste from mono_object_isinst (): keep in sync */
	if (check_interfaces && (klassc->flags & TYPE_ATTRIBUTE_INTERFACE) && !(klass->flags & TYPE_ATTRIBUTE_INTERFACE)) {
		MonoVTable *klass_vt = mono_class_vtable (domain, klass);
		if ((klassc->interface_id <= klass->max_interface_id) &&
		    klass_vt->interface_offsets [klassc->interface_id])
			return 1;
	} else if (check_interfaces && (klassc->flags & TYPE_ATTRIBUTE_INTERFACE) && (klass->flags & TYPE_ATTRIBUTE_INTERFACE)) {
		int i;

		for (i = 0; i < klass->interface_count; i ++) {
			MonoClass *ic =  klass->interfaces [i];
			if (ic == klassc)
				return 1;
		}
	} else {
		/*
		 * klass->baseval is 0 for interfaces 
		 */
		if (klass->baseval && ((klass->baseval - klassc->baseval) <= klassc->diffval))
			return 1;
	}
	return 0;
}

static guint32
ves_icall_get_attributes (MonoReflectionType *type)
{
	MonoClass *klass = mono_class_from_mono_type (type->type);

	return klass->flags;
}

static void
ves_icall_get_method_info (MonoMethod *method, MonoMethodInfo *info)
{
	MonoDomain *domain = mono_domain_get (); 

	info->parent = mono_type_get_object (domain, &method->klass->byval_arg);
	info->ret = mono_type_get_object (domain, method->signature->ret);
	info->name = mono_string_new (domain, method->name);
	info->attrs = method->flags;
	info->implattrs = method->iflags;
}

static MonoArray*
ves_icall_get_parameter_info (MonoMethod *method)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoArray *res;
	static MonoClass *System_Reflection_ParameterInfo;
	MonoReflectionParameter** args;
	int i;

	args = mono_param_get_objects (domain, method);
	if (!System_Reflection_ParameterInfo)
		System_Reflection_ParameterInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "ParameterInfo");
	res = mono_array_new (domain, System_Reflection_ParameterInfo, method->signature->param_count);
	for (i = 0; i < method->signature->param_count; ++i) {
		mono_array_set (res, gpointer, i, args [i]);
	}
	return res;
}

static void
ves_icall_get_field_info (MonoReflectionField *field, MonoFieldInfo *info)
{
	MonoDomain *domain = mono_domain_get (); 

	info->parent = mono_type_get_object (domain, &field->klass->byval_arg);
	info->type = mono_type_get_object (domain, field->field->type);
	info->name = mono_string_new (domain, field->field->name);
	info->attrs = field->field->type->attrs;
}

static MonoObject *
ves_icall_MonoField_GetValue (MonoReflectionField *field, MonoObject *obj) {
	MonoObject *res;
	MonoClass *klass;
	MonoType *ftype = field->field->type;
	int type = ftype->type;
	char *p, *r;
	guint32 align;

	mono_class_init (field->klass);
	if (ftype->attrs & FIELD_ATTRIBUTE_STATIC) {
		MonoVTable *vtable;
		vtable = mono_class_vtable (mono_domain_get (), field->klass);
		p = (char*)(vtable->data) + field->field->offset;
	} else {
		p = (char*)obj + field->field->offset;
	}

	switch (type) {
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return *(MonoObject**)p;
	}
	klass = mono_class_from_mono_type (ftype);
	res = mono_object_new (mono_domain_get (), klass);
	r = (char*)res + sizeof (MonoObject);
	memcpy (r, p, mono_class_value_size (klass, &align));

	return res;
}

static void
ves_icall_get_property_info (MonoReflectionProperty *property, MonoPropertyInfo *info)
{
	MonoDomain *domain = mono_domain_get (); 

	info->parent = mono_type_get_object (domain, &property->klass->byval_arg);
	info->name = mono_string_new (domain, property->property->name);
	info->attrs = property->property->attrs;
	info->get = property->property->get ? mono_method_get_object (domain, property->property->get): NULL;
	info->set = property->property->set ? mono_method_get_object (domain, property->property->set): NULL;
	/* 
	 * There may be other methods defined for properties, though, it seems they are not exposed 
	 * in the reflection API 
	 */
}

static MonoArray*
ves_icall_Type_GetInterfaces (MonoReflectionType* type)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoArray *intf;
	int ninterf, i;
	MonoClass *class = mono_class_from_mono_type (type->type);
	MonoClass *parent;

	ninterf = 0;
	for (parent = class; parent; parent = parent->parent) {
		ninterf += parent->interface_count;
	}
	intf = mono_array_new (domain, mono_defaults.monotype_class, ninterf);
	ninterf = 0;
	for (parent = class; parent; parent = parent->parent) {
		for (i = 0; i < parent->interface_count; ++i) {
			mono_array_set (intf, gpointer, ninterf, mono_type_get_object (domain, &parent->interfaces [i]->byval_arg));
			++ninterf;
		}
	}
	return intf;
}

static void
ves_icall_get_type_info (MonoType *type, MonoTypeInfo *info)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *class = mono_class_from_mono_type (type);

	info->parent = class->parent ? mono_type_get_object (domain, &class->parent->byval_arg): NULL;
	info->name = mono_string_new (domain, class->name);
	info->name_space = mono_string_new (domain, class->name_space);
	info->attrs = class->flags;
	info->rank = class->rank;
	info->assembly = NULL; /* FIXME */
	if (class->enumtype)
		info->etype = mono_type_get_object (domain, class->enum_basetype);
	else if (class->element_class)
		info->etype = mono_type_get_object (domain, &class->element_class->byval_arg);
	else
		info->etype = NULL;

	info->isbyref = type->byref;
	info->ispointer = type->type == MONO_TYPE_PTR;
	info->isprimitive = (type->type >= MONO_TYPE_BOOLEAN) && (type->type <= MONO_TYPE_R8);
}

static MonoObject*
ves_icall_InternalInvoke (MonoReflectionMethod *method, MonoObject *this, MonoArray *params) {
	//MonoMethodSignature *sig = method->method->signature;

	/*
	 * Do we need to copy the values so that the called method can't change them?
	 */

	return NULL;
}

static MonoObject *
ves_icall_System_Enum_ToObject (MonoReflectionType *type, MonoObject *obj)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *enumc;
	gint32 s1, s2;
	MonoObject *res;
	
	MONO_CHECK_ARG_NULL (type);
	MONO_CHECK_ARG_NULL (obj);

	enumc = mono_class_from_mono_type (type->type);

	MONO_CHECK_ARG (obj, enumc->enumtype == TRUE);
	MONO_CHECK_ARG (obj, obj->vtable->klass->byval_arg.type >= MONO_TYPE_I1 &&  
			obj->vtable->klass->byval_arg.type <= MONO_TYPE_U8);

	
	s1 = mono_class_value_size (enumc, NULL);
	s2 = mono_class_value_size (obj->vtable->klass, NULL);

	res = mono_object_new (domain, enumc);

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	memcpy ((gpointer)res + sizeof (MonoObject), (gpointer)obj + sizeof (MonoObject), MIN (s1, s2));
#else
	memcpy ((gpointer)res + sizeof (MonoObject) + (s1 > s2 ? s1 - s2 : 0),
		(gpointer)obj + sizeof (MonoObject) + (s2 > s1 ? s2 - s1 : 0),
		MIN (s1, s2));
#endif
	return res;
}

static MonoObject *
ves_icall_System_Enum_get_value (MonoObject *this)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoObject *res;
	MonoClass *enumc;
	gpointer dst;
	gpointer src;
	int size;

	if (!this)
		return NULL;

	g_assert (this->vtable->klass->enumtype);
	
	enumc = mono_class_from_mono_type (this->vtable->klass->enum_basetype);
	res = mono_object_new (domain, enumc);
	dst = (gpointer)res + sizeof (MonoObject);
	src = (gpointer)this + sizeof (MonoObject);
	size = mono_class_value_size (enumc, NULL);

	memcpy (dst, src, size);

	return res;
}

static void
ves_icall_get_enum_info (MonoReflectionType *type, MonoEnumInfo *info)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *enumc = mono_class_from_mono_type (type->type);
	guint i, j, nvalues, crow;
	MonoClassField *field;
	
	info->utype = mono_type_get_object (domain, enumc->enum_basetype);
	nvalues = enumc->field.count - 1;
	info->names = mono_array_new (domain, mono_defaults.string_class, nvalues);
	info->values = mono_array_new (domain, mono_class_from_mono_type (enumc->enum_basetype), nvalues);
	
	for (i = 0, j = 0; i < enumc->field.count; ++i) {
		field = &enumc->fields [i];
		if (strcmp ("value__", field->name) == 0)
			continue;
		mono_array_set (info->names, gpointer, j, mono_string_new (domain, field->name));
		if (!field->data) {
			crow = mono_metadata_get_constant_index (enumc->image, MONO_TOKEN_FIELD_DEF | (i+enumc->field.first+1));
			crow = mono_metadata_decode_row_col (&enumc->image->tables [MONO_TABLE_CONSTANT], crow-1, MONO_CONSTANT_VALUE);
			/* 1 is the length of the blob */
			field->data = 1 + mono_metadata_blob_heap (enumc->image, crow);
		}
		switch (enumc->enum_basetype->type) {
		case MONO_TYPE_U1:
		case MONO_TYPE_I1:
			mono_array_set (info->values, gchar, j, *field->data);
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_U2:
		case MONO_TYPE_I2:
			mono_array_set (info->values, gint16, j, read16 (field->data));
			break;
		case MONO_TYPE_U4:
		case MONO_TYPE_I4:
			mono_array_set (info->values, gint32, j, read32 (field->data));
			break;
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
			mono_array_set (info->values, gint64, j, read64 (field->data));
			break;
		default:
			g_error ("Implement type 0x%02x in get_enum_info", enumc->enum_basetype->type);
		}
		++j;
	}
}

static MonoMethod*
search_method (MonoReflectionType *type, char *name, guint32 flags, MonoArray *args)
{
	MonoClass *klass, *start_class;
	MonoMethod *m;
	MonoReflectionType *paramt;
	int i, j;

	start_class = klass = mono_class_from_mono_type (type->type);
	while (klass) {
		for (i = 0; i < klass->method.count; ++i) {
			m = klass->methods [i];
			if (!((m->flags & flags) == flags))
				continue;
			if (strcmp(m->name, name))
				continue;
			if (m->signature->param_count != mono_array_length (args))
				continue;
			for (j = 0; j < m->signature->param_count; ++j) {
				paramt = mono_array_get (args, MonoReflectionType*, j);
				if (!mono_metadata_type_equal (paramt->type, m->signature->params [j]))
					break;
			}
			if (j == m->signature->param_count)
				return m;
		}
		klass = klass->parent;
	}
	g_print ("Method %s.%s::%s (%d) not found\n", start_class->name_space, start_class->name, name, mono_array_length (args));
	return NULL;
}

static MonoReflectionMethod*
ves_icall_get_constructor (MonoReflectionType *type, MonoArray *args)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoMethod *m;

	m = search_method (type, ".ctor", METHOD_ATTRIBUTE_RT_SPECIAL_NAME, args);
	if (m)
		return mono_method_get_object (domain, m);
	return NULL;
}

static MonoReflectionMethod*
ves_icall_get_method (MonoReflectionType *type, MonoString *name, MonoArray *args)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoMethod *m;
	char *n = mono_string_to_utf8 (name);

	m = search_method (type, n, 0, args);
	g_free (n);
	if (m)
		return mono_method_get_object (domain, m);
	return NULL;
}

static MonoProperty*
search_property (MonoClass *klass, char* name, MonoArray *args) {
	int i;
	MonoProperty *p;

	/* FIXME: handle args */
	for (i = 0; i < klass->property.count; ++i) {
		p = &klass->properties [i];
		if (strcmp (p->name, name) == 0)
			return p;
	}
	return NULL;
}

static MonoReflectionProperty*
ves_icall_get_property (MonoReflectionType *type, MonoString *name, MonoArray *args)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoProperty *p;
	MonoClass *class = mono_class_from_mono_type (type->type);
	char *n = mono_string_to_utf8 (name);

	p = search_property (class, n, args);
	g_free (n);
	if (p)
		return mono_property_get_object (domain, class, p);
	return NULL;
}

enum {
	BFLAGS_IgnoreCase = 1,
	BFLAGS_DeclaredOnly = 2,
	BFLAGS_Instance = 4,
	BFLAGS_Static = 8,
	BFLAGS_Public = 0x10,
	BFLAGS_NonPublic = 0x20,
	BFLAGS_InvokeMethod = 0x100,
	BFLAGS_CreateInstance = 0x200,
	BFLAGS_GetField = 0x400,
	BFLAGS_SetField = 0x800,
	BFLAGS_GetProperty = 0x1000,
	BFLAGS_SetProperty = 0x2000,
	BFLAGS_ExactBinding = 0x10000,
	BFLAGS_SuppressChangeType = 0x20000,
	BFLAGS_OptionalParamBinding = 0x40000
};

static MonoArray*
ves_icall_Type_GetFields (MonoReflectionType *type, guint32 bflags)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_FieldInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoObject *member;
	int i, len, match;
	MonoClassField *field;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

handle_parent:	
	for (i = 0; i < klass->field.count; ++i) {
		match = 0;
		field = &klass->fields [i];
		if ((field->type->attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK) == FIELD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (field->type->attrs & FIELD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		member = (MonoObject*)mono_field_get_object (domain, klass, field);
		l = g_slist_prepend (l, member);
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	if (!System_Reflection_FieldInfo)
		System_Reflection_FieldInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "FieldInfo");
	res = mono_array_new (domain, System_Reflection_FieldInfo, len);
	i = 0;
	tmp = l;
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoArray*
ves_icall_Type_GetMethods (MonoReflectionType *type, guint32 bflags)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_MethodInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoMethod *method;
	MonoObject *member;
	int i, len, match;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

handle_parent:	
	for (i = 0; i < klass->method.count; ++i) {
		match = 0;
		method = klass->methods [i];
		if (strcmp (method->name, ".ctor") == 0 || strcmp (method->name, ".cctor") == 0)
			continue;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		match = 0;
		member = (MonoObject*)mono_method_get_object (domain, method);
			
		l = g_slist_prepend (l, member);
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	if (!System_Reflection_MethodInfo)
		System_Reflection_MethodInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "MethodInfo");
	res = mono_array_new (domain, System_Reflection_MethodInfo, len);
	i = 0;
	tmp = l;
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoArray*
ves_icall_Type_GetConstructors (MonoReflectionType *type, guint32 bflags)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_ConstructorInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoMethod *method;
	MonoObject *member;
	int i, len, match;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

handle_parent:	
	for (i = 0; i < klass->method.count; ++i) {
		match = 0;
		method = klass->methods [i];
		if (strcmp (method->name, ".ctor") && strcmp (method->name, ".cctor"))
			continue;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		member = (MonoObject*)mono_method_get_object (domain, method);
			
		l = g_slist_prepend (l, member);
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	if (!System_Reflection_ConstructorInfo)
		System_Reflection_ConstructorInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "ConstructorInfo");
	res = mono_array_new (domain, System_Reflection_ConstructorInfo, len);
	i = 0;
	tmp = l;
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static MonoArray*
ves_icall_Type_GetProperties (MonoReflectionType *type, guint32 bflags)
{
	MonoDomain *domain; 
	GSList *l = NULL, *tmp;
	static MonoClass *System_Reflection_PropertyInfo;
	MonoClass *startklass, *klass;
	MonoArray *res;
	MonoMethod *method;
	MonoProperty *prop;
	int i, len, match;

	domain = ((MonoObject *)type)->vtable->domain;
	klass = startklass = mono_class_from_mono_type (type->type);

handle_parent:	
	for (i = 0; i < klass->property.count; ++i) {
		prop = &klass->properties [i];
		match = 0;
		method = prop->get;
		if (!method)
			method = prop->set;
		if ((method->flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) == METHOD_ATTRIBUTE_PUBLIC) {
			if (bflags & BFLAGS_Public)
				match++;
		} else {
			if (bflags & BFLAGS_NonPublic)
				match++;
		}
		if (!match)
			continue;
		match = 0;
		if (method->flags & METHOD_ATTRIBUTE_STATIC) {
			if (bflags & BFLAGS_Static)
				match++;
		} else {
			if (bflags & BFLAGS_Instance)
				match++;
		}

		if (!match)
			continue;
		match = 0;
		l = g_slist_prepend (l, mono_property_get_object (domain, klass, prop));
	}
	if (!(bflags & BFLAGS_DeclaredOnly) && (klass = klass->parent))
		goto handle_parent;
	len = g_slist_length (l);
	if (!System_Reflection_PropertyInfo)
		System_Reflection_PropertyInfo = mono_class_from_name (
			mono_defaults.corlib, "System.Reflection", "PropertyInfo");
	res = mono_array_new (domain, System_Reflection_PropertyInfo, len);
	i = 0;
	tmp = l;
	for (; tmp; tmp = tmp->next, ++i)
		mono_array_set (res, gpointer, i, tmp->data);
	g_slist_free (l);
	return res;
}

static gpointer
ves_icall_System_Runtime_InteropServices_Marshal_ReadIntPtr (gpointer ptr)
{
	return (gpointer)(*(int *)ptr);
}

static MonoString*
ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAuto (gpointer ptr)
{
	MonoDomain *domain = mono_domain_get (); 

	return mono_string_new (domain, (char *)ptr);
}

static guint32 ves_icall_System_Runtime_InteropServices_Marshal_GetLastWin32Error(void)
{
	return(GetLastError());
}

static MonoReflectionType*
ves_icall_System_Reflection_Assembly_GetType (MonoReflectionAssembly *assembly, MonoString *type, MonoBoolean throwOnError, MonoBoolean ignoreCase)
{
	MonoDomain *domain = mono_domain_get (); 
	/* FIXME : use ignoreCase */
	gchar *name, *namespace, *str;
	char *byref, *isarray, *ispointer;
	guint rank;
	MonoClass *klass;

	str = namespace = mono_string_to_utf8 (type);
	/*g_print ("requested type %s in %s\n", str, assembly->assembly->name);*/

	name = strrchr (str, '.');
	byref = strrchr (str, '&');
	ispointer = strrchr (str, '*');
	if (byref)
		*byref = 0;
	if (ispointer)
		*ispointer = 0;
	isarray = strrchr (str, '[');
	if (isarray) {
		rank = 1;
		*isarray = 0;
		while (*isarray) {
			if (*isarray == ',')
				rank++;
			if (*isarray == ']')
				break;
			++isarray;
		}
	}

	if (name) {
		*name = 0;
		++name;
	} else {
		namespace = "";
		name = str;
	}

	klass = mono_class_from_name (assembly->assembly->image, namespace, name);
	g_free (str);
	if (!klass) {
		if (throwOnError)
			mono_raise_exception (mono_get_exception_type_load ());
		return NULL;
	}
	if (!klass->inited)
		mono_class_init (klass);

	if (isarray) {
		klass = mono_array_class_get (&klass->byval_arg, rank);
		mono_class_init (klass);
		/*g_print ("got array class %s [%d] (0x%x)\n", klass->element_class->name, klass->rank, klass->this_arg.type);*/
	}

	if (byref || ispointer)
		return mono_type_get_object (domain, &klass->this_arg);
	else
		return mono_type_get_object (domain, &klass->byval_arg);
}

static MonoString *
ves_icall_System_Reflection_Assembly_get_code_base (MonoReflectionAssembly *assembly)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoString *res;
	char *name = g_strconcat (
		"file://", assembly->assembly->image->name, NULL);
	
	res = mono_string_new (domain, name);
	g_free (name);
	return res;
}

static MonoString *
ves_icall_System_MonoType_assQualifiedName (MonoReflectionType *object)
{
	MonoDomain *domain = mono_domain_get (); 
	/* FIXME : real rules are more complicated (internal classes,
	  reference types, array types, etc. */
	MonoString *res;
	gchar *fullname;
	MonoClass *klass;
	char *append = NULL;

	switch (object->type->type) {
	case MONO_TYPE_ARRAY: {
		int i, rank = object->type->data.array->rank;
		char *tmp, *str = g_strdup ("[");
		klass = mono_class_from_mono_type (object->type->data.array->type);
		for (i = 1; i < rank; i++) {
			tmp = g_strconcat (str, ",", NULL);
			g_free (str);
			str = tmp;
		}
		tmp = g_strconcat (str, "]", NULL);
		g_free (str);
		append = tmp;
		break;
	}
	case MONO_TYPE_SZARRAY:
		klass = mono_class_from_mono_type (object->type->data.type);
		append = g_strdup ("[]");
		break;
	case MONO_TYPE_PTR:
		klass = mono_class_from_mono_type (object->type->data.type);
		append = g_strdup ("*");
		break;
	default:
		klass = object->type->data.klass;
		break;
	}

	fullname = g_strconcat (klass->name_space, ".",
	                           klass->name, append, ",",
	                           klass->image->assembly_name, NULL);
	res = mono_string_new (domain, fullname);
	g_free (fullname);
	g_free (append);

	return res;
}

static MonoReflectionType*
ves_icall_ModuleBuilder_create_modified_type (MonoReflectionType *tb, gint32 arrayrank, MonoBoolean isbyref)
{
	MonoClass *klass;

	if (arrayrank)
		klass = mono_array_class_get (tb->type, arrayrank);
	else
		klass = mono_class_from_mono_type (tb->type);

	return mono_type_get_object (mono_domain_get (), isbyref? &klass->this_arg: &klass->byval_arg);
}

static MonoString *
ves_icall_System_PAL_GetCurrentDirectory (MonoObject *object)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoString *res;
	gchar *path = g_get_current_dir ();
	res = mono_string_new (domain, path);
	g_free (path);
	return res;
}

/*
 * Magic number to convert a time which is relative to
 * Jan 1, 1970 into a value which is relative to Jan 1, 0001.
 */
#define	EPOCH_ADJUST	((gint64)62135596800L)

static gint64
ves_icall_System_DateTime_GetNow ()
{
#ifndef PLATFORM_WIN32
	/* FIXME: put this in io-layer and call it GetLocalTime */
	struct timeval tv;
	gint64 res;

	if (gettimeofday (&tv, NULL) == 0) {
		res = (((gint64)tv.tv_sec + EPOCH_ADJUST)* 1000000 + tv.tv_usec)*10;
		return res;
	}

	/* fixme: raise exception */
#endif
	return 0;
}

/*
 * This is heavily based on zdump.c from glibc 2.2.
 *
 *  * data[0]:  start of daylight saving time (in DateTime ticks).
 *  * data[1]:  end of daylight saving time (in DateTime ticks).
 *  * data[2]:  utcoffset (in TimeSpan ticks).
 *  * data[3]:  additional offset when daylight saving (in TimeSpan ticks).
 *  * name[0]:  name of this timezone when not daylight saving.
 *  * name[1]:  name of this timezone when daylight saving.
 *
 *  FIXME: This only works with "standard" Unix dates (years between 1900 and 2100) while
 *         the class library allows years between 1 and 9999.
 *
 *  Returns true on success and zero on failure.
 */
static guint32
ves_icall_System_CurrentTimeZone_GetTimeZoneData (guint32 year, MonoArray **data, MonoArray **names)
{
#ifndef PLATFORM_WIN32
	MonoDomain *domain = mono_domain_get ();
	struct tm start, tt;
	time_t t;

	long int gmtoff;
	int is_daylight = 0, day;

	memset (&start, 0, sizeof (start));

	start.tm_mday = 1;
	start.tm_year = year-1900;

	t = mktime (&start);
	gmtoff = start.tm_gmtoff;

	MONO_CHECK_ARG_NULL (data);
	MONO_CHECK_ARG_NULL (names);

	(*data) = mono_array_new (domain, mono_defaults.int64_class, 4);
	(*names) = mono_array_new (domain, mono_defaults.string_class, 2);

	/* For each day of the year, calculate the tm_gmtoff. */
	for (day = 0; day < 365; day++) {

		t += 3600*24;
		tt = *localtime (&t);

		/* Daylight saving starts or ends here. */
		if (tt.tm_gmtoff != gmtoff) {
			struct tm tt1;
			time_t t1;

			/* Try to find the exact hour when daylight saving starts/ends. */
			t1 = t;
			do {
				t1 -= 3600;
				tt1 = *localtime (&t1);
			} while (tt1.tm_gmtoff != gmtoff);

			/* Try to find the exact minute when daylight saving starts/ends. */
			do {
				t1 += 60;
				tt1 = *localtime (&t1);
			} while (tt1.tm_gmtoff == gmtoff);

			/* Write data, if we're already in daylight saving, we're done. */
			if (is_daylight) {
				mono_array_set ((*names), gpointer, 0, mono_string_new (domain, tt.tm_zone));
				mono_array_set ((*data), gint64, 1, ((gint64)t1 + EPOCH_ADJUST) * 10000000L);
				return 1;
			} else {
				mono_array_set ((*names), gpointer, 1, mono_string_new (domain, tt.tm_zone));
				mono_array_set ((*data), gint64, 0, ((gint64)t1 + EPOCH_ADJUST) * 10000000L);
				is_daylight = 1;
			}

			/* This is only set once when we enter daylight saving. */
			mono_array_set ((*data), gint64, 2, (gint64)gmtoff * 10000000L);
			mono_array_set ((*data), gint64, 3, (gint64)(tt.tm_gmtoff - gmtoff) * 10000000L);

			gmtoff = tt.tm_gmtoff;
		}

		gmtoff = tt.tm_gmtoff;
	}

	return 0;
#endif
}


static gpointer
ves_icall_System_Object_obj_address (MonoObject *this) {
	return this;
}

static gpointer icall_map [] = {
	/*
	 * System.Array
	 */
	"System.Array::GetValue",         ves_icall_System_Array_GetValue,
	"System.Array::SetValue",         ves_icall_System_Array_SetValue,
	"System.Array::GetValueImpl",     ves_icall_System_Array_GetValueImpl,
	"System.Array::SetValueImpl",     ves_icall_System_Array_SetValueImpl,
	"System.Array::GetRank",          ves_icall_System_Array_GetRank,
	"System.Array::GetLength",        ves_icall_System_Array_GetLength,
	"System.Array::GetLowerBound",    ves_icall_System_Array_GetLowerBound,
	"System.Array::CreateInstanceImpl",   ves_icall_System_Array_CreateInstanceImpl,
	"System.Array::FastCopy",         ves_icall_System_Array_FastCopy,
	"System.Array::Clone",            mono_array_clone,

	/*
	 * System.Object
	 */
	"System.Object::MemberwiseClone", ves_icall_System_Object_MemberwiseClone,
	"System.Object::GetType", ves_icall_System_Object_GetType,
	"System.Object::obj_address", ves_icall_System_Object_obj_address,

	/*
	 * System.String
	 */
	"System.String::_IsInterned", mono_string_is_interned,
	"System.String::_Intern", mono_string_intern,

	/*
	 * System.AppDomain
	 */
	"System.AppDomain::createDomain", ves_icall_System_AppDomain_createDomain,
	"System.AppDomain::getCurDomain", ves_icall_System_AppDomain_getCurDomain,
	"System.AppDomain::GetData", ves_icall_System_AppDomain_GetData,
	"System.AppDomain::SetData", ves_icall_System_AppDomain_SetData,
	"System.AppDomain::getSetup", ves_icall_System_AppDomain_getSetup,
	"System.AppDomain::getFriendlyName", ves_icall_System_AppDomain_getFriendlyName,
	"System.AppDomain::GetAssemblies", ves_icall_System_AppDomain_GetAssemblies,
	"System.AppDomain::LoadAssembly", ves_icall_System_AppDomain_LoadAssembly,
	"System.AppDomain::Unload", ves_icall_System_AppDomain_Unload,
	"System.AppDomain::ExecuteAssembly", ves_icall_System_AppDomain_ExecuteAssembly,

	/*
	 * System.AppDomainSetup
	 */
	"System.AppDomainSetup::InitAppDomainSetup", ves_icall_System_AppDomainSetup_InitAppDomainSetup,

	/*
	 * System.Decimal
	 */
	"System.Decimal::decimal2UInt64", mono_decimal2UInt64,
	"System.Decimal::decimal2Int64", mono_decimal2Int64,
	"System.Decimal::double2decimal", mono_double2decimal, /* FIXME: wrong signature. */
	"System.Decimal::decimalIncr", mono_decimalIncr,
	"System.Decimal::decimalSetExponent", mono_decimalSetExponent,
	"System.Decimal::decimal2double", mono_decimal2double,
	"System.Decimal::decimalFloorAndTrunc", mono_decimalFloorAndTrunc,
	"System.Decimal::decimalRound", mono_decimalRound,
	"System.Decimal::decimalMult", mono_decimalMult,
	"System.Decimal::decimalDiv", mono_decimalDiv,
	"System.Decimal::decimalIntDiv", mono_decimalIntDiv,
	"System.Decimal::decimalCompare", mono_decimalCompare,
	"System.Decimal::string2decimal", mono_string2decimal,
	"System.Decimal::decimal2string", mono_decimal2string,

	/*
	 * ModuleBuilder
	 */
	"System.Reflection.Emit.ModuleBuilder::create_modified_type", ves_icall_ModuleBuilder_create_modified_type,
	
	/*
	 * AssemblyBuilder
	 */
	"System.Reflection.Emit.AssemblyBuilder::getDataChunk", ves_icall_get_data_chunk,
	"System.Reflection.Emit.AssemblyBuilder::getUSIndex", mono_image_insert_string,
	"System.Reflection.Emit.AssemblyBuilder::getToken", ves_icall_AssemblyBuilder_getToken,
	"System.Reflection.Emit.AssemblyBuilder::basic_init", mono_image_basic_init,

	/*
	 * Reflection stuff.
	 */
	"System.Reflection.MonoMethodInfo::get_method_info", ves_icall_get_method_info,
	"System.Reflection.MonoMethodInfo::get_parameter_info", ves_icall_get_parameter_info,
	"System.Reflection.MonoFieldInfo::get_field_info", ves_icall_get_field_info,
	"System.Reflection.MonoPropertyInfo::get_property_info", ves_icall_get_property_info,
	"System.Reflection.MonoMethod::InternalInvoke", ves_icall_InternalInvoke,
	"System.MonoCustomAttrs::GetCustomAttributes", mono_reflection_get_custom_attrs,
	"System.Reflection.Emit.CustomAttributeBuilder::GetBlob", mono_reflection_get_custom_attrs_blob,
	"System.Reflection.MonoField::GetValue", ves_icall_MonoField_GetValue,
	
	/* System.Enum */

	"System.MonoEnumInfo::get_enum_info", ves_icall_get_enum_info,
	"System.Enum::get_value", ves_icall_System_Enum_get_value,
	"System.Enum::ToObject", ves_icall_System_Enum_ToObject,

	/*
	 * TypeBuilder
	 */
	"System.Reflection.Emit.TypeBuilder::setup_internal_class", mono_reflection_setup_internal_class,
	
	/*
	 * MethodBuilder
	 */
	
	/*
	 * System.Type
	 */
	"System.Type::internal_from_name", ves_icall_type_from_name,
	"System.Type::internal_from_handle", ves_icall_type_from_handle,
	"System.Type::get_constructor", ves_icall_get_constructor,
	"System.Type::get_property", ves_icall_get_property,
	"System.Type::get_method", ves_icall_get_method,
	"System.MonoType::get_attributes", ves_icall_get_attributes,
	"System.Type::type_is_subtype_of", ves_icall_type_is_subtype_of,
	"System.Type::Equals", ves_icall_type_Equals,

	/*
	 * System.Runtime.CompilerServices.RuntimeHelpers
	 */
	"System.Runtime.CompilerServices.RuntimeHelpers::InitializeArray", ves_icall_InitializeArray,
	
	/*
	 * System.Threading
	 */
	"System.Threading.Thread::Thread_internal", ves_icall_System_Threading_Thread_Thread_internal,
	"System.Threading.Thread::Start_internal", ves_icall_System_Threading_Thread_Start_internal,
	"System.Threading.Thread::Sleep_internal", ves_icall_System_Threading_Thread_Sleep_internal,
	"System.Threading.Thread::CurrentThread_internal", ves_icall_System_Threading_Thread_CurrentThread_internal,
	"System.Threading.Thread::Join_internal", ves_icall_System_Threading_Thread_Join_internal,
	"System.Threading.Thread::SlotHash_lookup", ves_icall_System_Threading_Thread_SlotHash_lookup,
	"System.Threading.Thread::SlotHash_store", ves_icall_System_Threading_Thread_SlotHash_store,
	"System.Threading.Monitor::Monitor_exit", ves_icall_System_Threading_Monitor_Monitor_exit,
	"System.Threading.Monitor::Monitor_test_owner", ves_icall_System_Threading_Monitor_Monitor_test_owner,
	"System.Threading.Monitor::Monitor_test_synchronised", ves_icall_System_Threading_Monitor_Monitor_test_synchronised,
	"System.Threading.Monitor::Monitor_pulse", ves_icall_System_Threading_Monitor_Monitor_pulse,
	"System.Threading.Monitor::Monitor_pulse_all", ves_icall_System_Threading_Monitor_Monitor_pulse_all,
	"System.Threading.Monitor::Monitor_try_enter", ves_icall_System_Threading_Monitor_Monitor_try_enter,
	"System.Threading.Monitor::Monitor_wait", ves_icall_System_Threading_Monitor_Monitor_wait,
	"System.Threading.Mutex::CreateMutex_internal", ves_icall_System_Threading_Mutex_CreateMutex_internal,
	"System.Threading.Mutex::ReleaseMutex_internal", ves_icall_System_Threading_Mutex_ReleaseMutex_internal,
	"System.Threading.NativeEventCalls::CreateEvent_internal", ves_icall_System_Threading_Events_CreateEvent_internal,
	"System.Threading.NativeEventCalls::SetEvent_internal",    ves_icall_System_Threading_Events_SetEvent_internal,
	"System.Threading.NativeEventCalls::ResetEvent_internal",  ves_icall_System_Threading_Events_ResetEvent_internal,

	/*
	 * System.Threading.WaitHandle
	 */
	"System.Threading.WaitHandle::WaitAll_internal", ves_icall_System_Threading_WaitHandle_WaitAll_internal,
	"System.Threading.WaitHandle::WaitAny_internal", ves_icall_System_Threading_WaitHandle_WaitAny_internal,
	"System.Threading.WaitHandle::WaitOne_internal", ves_icall_System_Threading_WaitHandle_WaitOne_internal,

	"System.Runtime.InteropServices.Marshal::ReadIntPtr", ves_icall_System_Runtime_InteropServices_Marshal_ReadIntPtr,
	"System.Runtime.InteropServices.Marshal::PtrToStringAuto", ves_icall_System_Runtime_InteropServices_Marshal_PtrToStringAuto,
	"System.Runtime.InteropServices.Marshal::GetLastWin32Error", ves_icall_System_Runtime_InteropServices_Marshal_GetLastWin32Error,

	"System.Reflection.Assembly::GetType", ves_icall_System_Reflection_Assembly_GetType,
	"System.Reflection.Assembly::get_code_base", ves_icall_System_Reflection_Assembly_get_code_base,

	/*
	 * System.MonoType.
	 */
	"System.MonoType::assQualifiedName", ves_icall_System_MonoType_assQualifiedName,
	"System.MonoType::type_from_obj", mono_type_type_from_obj,
	"System.MonoType::get_type_info", ves_icall_get_type_info,
	"System.MonoType::GetFields", ves_icall_Type_GetFields,
	"System.MonoType::GetMethods", ves_icall_Type_GetMethods,
	"System.MonoType::GetConstructors", ves_icall_Type_GetConstructors,
	"System.MonoType::GetProperties", ves_icall_Type_GetProperties,
	"System.MonoType::GetInterfaces", ves_icall_Type_GetInterfaces,

	"System.PAL.OpSys::GetCurrentDirectory", ves_icall_System_PAL_GetCurrentDirectory,

	/*
	 * System.PAL.OpSys I/O Services
	 */
	"System.PAL.OpSys::GetStdHandle", ves_icall_System_PAL_OpSys_GetStdHandle,
	"System.PAL.OpSys::ReadFile", ves_icall_System_PAL_OpSys_ReadFile,
	"System.PAL.OpSys::WriteFile", ves_icall_System_PAL_OpSys_WriteFile,
	"System.PAL.OpSys::SetLengthFile", ves_icall_System_PAL_OpSys_SetLengthFile,
	"System.PAL.OpSys::OpenFile", ves_icall_System_PAL_OpSys_OpenFile,
	"System.PAL.OpSys::CloseFile", ves_icall_System_PAL_OpSys_CloseFile,
	"System.PAL.OpSys::SeekFile", ves_icall_System_PAL_OpSys_SeekFile,
	"System.PAL.OpSys::DeleteFile", ves_icall_System_PAL_OpSys_DeleteFile,
	"System.PAL.OpSys::ExistsFile", ves_icall_System_PAL_OpSys_ExistsFile,
	"System.PAL.OpSys::GetFileTime", ves_icall_System_PAL_OpSys_GetFileTime,
	"System.PAL.OpSys::SetFileTime", ves_icall_System_PAL_OpSys_SetFileTime,

	/*
	 * System.Net.Sockets I/O Services
	 */
	"System.Net.Sockets.Socket::Socket_internal", ves_icall_System_Net_Sockets_Socket_Socket_internal,
	"System.Net.Sockets.Socket::Close_internal", ves_icall_System_Net_Sockets_Socket_Close_internal,
	"System.Net.Sockets.SocketException::WSAGetLastError_internal", ves_icall_System_Net_Sockets_SocketException_WSAGetLastError_internal,
	"System.Net.Sockets.Socket::Available_internal", ves_icall_System_Net_Sockets_Socket_Available_internal,
	"System.Net.Sockets.Socket::Blocking_internal", ves_icall_System_Net_Sockets_Socket_Blocking_internal,
	"System.Net.Sockets.Socket::Accept_internal", ves_icall_System_Net_Sockets_Socket_Accept_internal,
	"System.Net.Sockets.Socket::Listen_internal", ves_icall_System_Net_Sockets_Socket_Listen_internal,
	"System.Net.Sockets.Socket::LocalEndPoint_internal", ves_icall_System_Net_Sockets_Socket_LocalEndPoint_internal,
	"System.Net.Sockets.Socket::RemoteEndPoint_internal", ves_icall_System_Net_Sockets_Socket_RemoteEndPoint_internal,
	"System.Net.Sockets.Socket::Bind_internal", ves_icall_System_Net_Sockets_Socket_Bind_internal,
	"System.Net.Sockets.Socket::Connect_internal", ves_icall_System_Net_Sockets_Socket_Connect_internal,
	"System.Net.Sockets.Socket::Receive_internal", ves_icall_System_Net_Sockets_Socket_Receive_internal,
	"System.Net.Sockets.Socket::RecvFrom_internal", ves_icall_System_Net_Sockets_Socket_RecvFrom_internal,
	"System.Net.Sockets.Socket::Send_internal", ves_icall_System_Net_Sockets_Socket_Send_internal,
	"System.Net.Sockets.Socket::SendTo_internal", ves_icall_System_Net_Sockets_Socket_SendTo_internal,
	"System.Net.Sockets.Socket::Select_internal", ves_icall_System_Net_Sockets_Socket_Select_internal,
	"System.Net.Sockets.Socket::Shutdown_internal", ves_icall_System_Net_Sockets_Socket_Shutdown_internal,
	"System.Net.Sockets.Socket::GetSocketOption_obj_internal", ves_icall_System_Net_Sockets_Socket_GetSocketOption_obj_internal,
	"System.Net.Sockets.Socket::GetSocketOption_arr_internal", ves_icall_System_Net_Sockets_Socket_GetSocketOption_arr_internal,
	"System.Net.Sockets.Socket::SetSocketOption_internal", ves_icall_System_Net_Sockets_Socket_SetSocketOption_internal,
	"System.Net.Dns::GetHostByName_internal", ves_icall_System_Net_Dns_GetHostByName_internal,
	"System.Net.Dns::GetHostByAddr_internal", ves_icall_System_Net_Dns_GetHostByAddr_internal,

	/*
	 * System.Char
	 */
	"System.Char::GetNumericValue", ves_icall_System_Char_GetNumericValue,
	"System.Char::GetUnicodeCategory", ves_icall_System_Char_GetUnicodeCategory,
	"System.Char::IsControl", ves_icall_System_Char_IsControl,
	"System.Char::IsDigit", ves_icall_System_Char_IsDigit,
	"System.Char::IsLetter", ves_icall_System_Char_IsLetter,
	"System.Char::IsLower", ves_icall_System_Char_IsLower,
	"System.Char::IsUpper", ves_icall_System_Char_IsUpper,
	"System.Char::IsNumber", ves_icall_System_Char_IsNumber,
	"System.Char::IsPunctuation", ves_icall_System_Char_IsPunctuation,
	"System.Char::IsSeparator", ves_icall_System_Char_IsSeparator,
	"System.Char::IsSurrogate", ves_icall_System_Char_IsSurrogate,
	"System.Char::IsSymbol", ves_icall_System_Char_IsSymbol,
	"System.Char::IsWhiteSpace", ves_icall_System_Char_IsWhiteSpace,
	"System.Char::ToLower", ves_icall_System_Char_ToLower,
	"System.Char::ToUpper", ves_icall_System_Char_ToUpper,

	"System.Text.Encoding::IConvNewEncoder", ves_icall_iconv_new_encoder,
	"System.Text.Encoding::IConvNewDecoder", ves_icall_iconv_new_decoder,
	"System.Text.Encoding::IConvReset", ves_icall_iconv_reset,
	"System.Text.Encoding::IConvGetByteCount", ves_icall_iconv_get_byte_count,
	"System.Text.Encoding::IConvGetBytes", ves_icall_iconv_get_bytes,
	"System.Text.Encoding::IConvGetCharCount", ves_icall_iconv_get_char_count,
	"System.Text.Encoding::IConvGetChars", ves_icall_iconv_get_chars,

	"System.DateTime::GetNow", ves_icall_System_DateTime_GetNow,
	"System.CurrentTimeZone::GetTimeZoneData", ves_icall_System_CurrentTimeZone_GetTimeZoneData,

	/*
	 * System.Security.Cryptography calls
	 */

	 "System.Security.Cryptography.RNGCryptoServiceProvider::GetBytes", ves_icall_System_Security_Cryptography_RNGCryptoServiceProvider_GetBytes,
	 "System.Security.Cryptography.RNG_CryptoServiceProvider::GetNonZeroBytes", ves_icall_System_Security_Cryptography_RNGCryptoServiceProvider_GetNonZeroBytes,

	/*
	 * add other internal calls here
	 */
	NULL, NULL
};

void
mono_init_icall ()
{
	char *n;
	int i = 0;

	while ((n = icall_map [i])) {
		mono_add_internal_call (n, icall_map [i+1]);
		i += 2;
	}
       
}


