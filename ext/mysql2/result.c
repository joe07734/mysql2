#include <mysql2_ext.h>
#include <stdint.h>

#ifdef HAVE_RUBY_ENCODING_H
static rb_encoding *binaryEncoding;
#endif

#if (SIZEOF_INT < SIZEOF_LONG) || defined(HAVE_RUBY_ENCODING_H)
/* on 64bit platforms we can handle dates way outside 2038-01-19T03:14:07
 *
 * (9999*31557600) + (12*2592000) + (31*86400) + (11*3600) + (59*60) + 59
 */
#define MYSQL2_MAX_TIME 315578267999ULL
#else
/**
 * On 32bit platforms the maximum date the Time class can handle is 2038-01-19T03:14:07
 * 2038 years + 1 month + 19 days + 3 hours + 14 minutes + 7 seconds = 64318634047 seconds
 *
 * (2038*31557600) + (1*2592000) + (19*86400) + (3*3600) + (14*60) + 7
 */
#define MYSQL2_MAX_TIME 64318634047ULL
#endif

#if defined(HAVE_RUBY_ENCODING_H)
/* 0000-1-1 00:00:00 UTC
 *
 * (0*31557600) + (1*2592000) + (1*86400) + (0*3600) + (0*60) + 0
 */
#define MYSQL2_MIN_TIME 2678400ULL
#elif SIZEOF_INT < SIZEOF_LONG // 64bit Ruby 1.8
/* 0139-1-1 00:00:00 UTC
 *
 * (139*31557600) + (1*2592000) + (1*86400) + (0*3600) + (0*60) + 0
 */
#define MYSQL2_MIN_TIME 4389184800ULL
#elif defined(NEGATIVE_TIME_T)
/* 1901-12-13 20:45:52 UTC : The oldest time in 32-bit signed time_t.
 *
 * (1901*31557600) + (12*2592000) + (13*86400) + (20*3600) + (45*60) + 52
 */
#define MYSQL2_MIN_TIME 60023299552ULL
#else
/* 1970-01-01 00:00:01 UTC : The Unix epoch - the oldest time in portable time_t.
 *
 * (1970*31557600) + (1*2592000) + (1*86400) + (0*3600) + (0*60) + 1
 */
#define MYSQL2_MIN_TIME 62171150401ULL
#endif

static VALUE cMysql2Result;
static VALUE cBigDecimal, cDate, cDateTime;
static VALUE opt_decimal_zero, opt_float_zero, opt_time_year, opt_time_month, opt_utc_offset;
extern VALUE mMysql2, cMysql2Client, cMysql2Error;
static VALUE intern_encoding_from_charset;
static ID intern_new, intern_utc, intern_local, intern_encoding_from_charset_code,
          intern_localtime, intern_local_offset, intern_civil, intern_new_offset;
static VALUE sym_symbolize_keys, sym_as, sym_array, sym_struct, sym_database_timezone, sym_application_timezone,
          sym_local, sym_utc, sym_cast_booleans, sym_cache_rows, sym_cast, sym_stream;
static ID intern_merge;

/* internal :as constants */
#define AS_HASH   0
#define AS_ARRAY  1
#define AS_STRUCT 2

/* wrapper for intern.h:rb_struct_define() */
static VALUE rb_mysql_struct_define2(const char *name, char **ary, int len);


static void rb_mysql_result_mark(void * wrapper) {
  mysql2_result_wrapper * w = wrapper;
  if (w) {
    rb_gc_mark(w->fields);
    rb_gc_mark(w->rows);
    rb_gc_mark(w->encoding);
    rb_gc_mark(w->asStruct);
  }
}

/* this may be called manually or during GC */
static void rb_mysql_result_free_result(mysql2_result_wrapper * wrapper) {
  if (wrapper && wrapper->resultFreed != 1) {
    mysql_free_result(wrapper->result);
    wrapper->resultFreed = 1;
  }
}

/* this is called during GC */
static void rb_mysql_result_free(void * wrapper) {
  mysql2_result_wrapper * w = wrapper;
  /* FIXME: this may call flush_use_result, which can hit the socket */
  rb_mysql_result_free_result(w);
  xfree(wrapper);
}

/*
 * for small results, this won't hit the network, but there's no
 * reliable way for us to tell this so we'll always release the GVL
 * to be safe
 */
static VALUE nogvl_fetch_row(void *ptr) {
  MYSQL_RES *result = ptr;

  return (VALUE)mysql_fetch_row(result);
}

static VALUE rb_mysql_result_fetch_field(VALUE self, unsigned int idx, short int symbolize_keys) {
  mysql2_result_wrapper * wrapper;
  VALUE rb_field;
  GetMysql2Result(self, wrapper);

  if (wrapper->fields == Qnil) {
    wrapper->numberOfFields = mysql_num_fields(wrapper->result);
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }

  rb_field = rb_ary_entry(wrapper->fields, idx);
  if (rb_field == Qnil) {
    MYSQL_FIELD *field = NULL;
#ifdef HAVE_RUBY_ENCODING_H
    rb_encoding *default_internal_enc = rb_default_internal_encoding();
    rb_encoding *conn_enc = rb_to_encoding(wrapper->encoding);
#endif

    field = mysql_fetch_field_direct(wrapper->result, idx);
    if (symbolize_keys) {
      VALUE colStr;
      char buf[field->name_length+1];
      memcpy(buf, field->name, field->name_length);
      buf[field->name_length] = 0;
      colStr = rb_str_new2(buf);
#ifdef HAVE_RUBY_ENCODING_H
      rb_enc_associate(colStr, rb_utf8_encoding());
#endif
      rb_field = ID2SYM(rb_to_id(colStr));
    } else {
      rb_field = rb_str_new(field->name, field->name_length);
#ifdef HAVE_RUBY_ENCODING_H
      rb_enc_associate(rb_field, conn_enc);
      if (default_internal_enc) {
        rb_field = rb_str_export_to_enc(rb_field, default_internal_enc);
      }
#endif
    }
    rb_ary_store(wrapper->fields, idx, rb_field);
  }

  return rb_field;
}

#ifdef HAVE_RUBY_ENCODING_H
static VALUE mysql2_set_field_string_encoding(VALUE val, MYSQL_FIELD field, rb_encoding *default_internal_enc, rb_encoding *conn_enc) {
  // if binary flag is set, respect it's wishes
  if (field.flags & BINARY_FLAG && field.charsetnr == 63) {
    rb_enc_associate(val, binaryEncoding);
  } else {
    // lookup the encoding configured on this field
    VALUE new_encoding = rb_funcall(cMysql2Client, intern_encoding_from_charset_code, 1, INT2NUM(field.charsetnr));
    if (new_encoding != Qnil) {
      // use the field encoding we were able to match
      rb_encoding *enc = rb_to_encoding(new_encoding);
      rb_enc_associate(val, enc);
    } else {
      // otherwise fall-back to the connection's encoding
      rb_enc_associate(val, conn_enc);
    }
    if (default_internal_enc) {
      val = rb_str_export_to_enc(val, default_internal_enc);
    }
  }
  return val;
}
#endif

static VALUE rb_mysql_result_fetch_row(VALUE self, ID db_timezone, ID app_timezone, int symbolizeKeys, int as, int castBool, int cast, MYSQL_FIELD * fields) {
  VALUE rowVal;
  mysql2_result_wrapper * wrapper;
  MYSQL_ROW row;
  unsigned int i = 0;
  unsigned long * fieldLengths;
  void * ptr;
#ifdef HAVE_RUBY_ENCODING_H
  rb_encoding *default_internal_enc;
  rb_encoding *conn_enc;
#endif
  GetMysql2Result(self, wrapper);

#ifdef HAVE_RUBY_ENCODING_H
  default_internal_enc = rb_default_internal_encoding();
  conn_enc = rb_to_encoding(wrapper->encoding);
#endif

  ptr = wrapper->result;
  row = (MYSQL_ROW)rb_thread_blocking_region(nogvl_fetch_row, ptr, RUBY_UBF_IO, 0);
  if (row == NULL) {
    return Qnil;
  }

  fieldLengths = mysql_fetch_lengths(wrapper->result);
  if (wrapper->fields == Qnil) {
    wrapper->numberOfFields = mysql_num_fields(wrapper->result);
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }

  if (as == AS_HASH) {
    rowVal = rb_hash_new();
  } else {
    rowVal = rb_ary_new2(wrapper->numberOfFields);
  }

  for (i = 0; i < wrapper->numberOfFields; i++) {
    VALUE field = rb_mysql_result_fetch_field(self, i, symbolizeKeys);
    if (row[i]) {
      VALUE val = Qnil;
      enum enum_field_types type = fields[i].type;

      if(!cast) {
        if (type == MYSQL_TYPE_NULL) {
          val = Qnil;
        } else {
          val = rb_str_new(row[i], fieldLengths[i]);
#ifdef HAVE_RUBY_ENCODING_H
          val = mysql2_set_field_string_encoding(val, fields[i], default_internal_enc, conn_enc);
#endif
        }
      } else {
        switch(type) {
        case MYSQL_TYPE_NULL:       // NULL-type field
          val = Qnil;
          break;
        case MYSQL_TYPE_BIT:        // BIT field (MySQL 5.0.3 and up)
          val = rb_str_new(row[i], fieldLengths[i]);
          break;
        case MYSQL_TYPE_TINY:       // TINYINT field
          if (castBool && fields[i].length == 1) {
            val = *row[i] == '1' ? Qtrue : Qfalse;
            break;
          }
        case MYSQL_TYPE_SHORT:      // SMALLINT field
        case MYSQL_TYPE_LONG:       // INTEGER field
        case MYSQL_TYPE_INT24:      // MEDIUMINT field
        case MYSQL_TYPE_LONGLONG:   // BIGINT field
        case MYSQL_TYPE_YEAR:       // YEAR field
          val = rb_cstr2inum(row[i], 10);
          break;
        case MYSQL_TYPE_DECIMAL:    // DECIMAL or NUMERIC field
        case MYSQL_TYPE_NEWDECIMAL: // Precision math DECIMAL or NUMERIC field (MySQL 5.0.3 and up)
          if (fields[i].decimals == 0) {
            val = rb_cstr2inum(row[i], 10);
          } else if (strtod(row[i], NULL) == 0.000000){
            val = rb_funcall(cBigDecimal, intern_new, 1, opt_decimal_zero);
          }else{
            val = rb_funcall(cBigDecimal, intern_new, 1, rb_str_new(row[i], fieldLengths[i]));
          }
          break;
        case MYSQL_TYPE_FLOAT:      // FLOAT field
        case MYSQL_TYPE_DOUBLE: {     // DOUBLE or REAL field
          double column_to_double;
          column_to_double = strtod(row[i], NULL);
          if (column_to_double == 0.000000){
            val = opt_float_zero;
          }else{
            val = rb_float_new(column_to_double);
          }
          break;
        }
        case MYSQL_TYPE_TIME: {     // TIME field
          int hour, min, sec, tokens;
          tokens = sscanf(row[i], "%2d:%2d:%2d", &hour, &min, &sec);
          val = rb_funcall(rb_cTime, db_timezone, 6, opt_time_year, opt_time_month, opt_time_month, INT2NUM(hour), INT2NUM(min), INT2NUM(sec));
          if (!NIL_P(app_timezone)) {
            if (app_timezone == intern_local) {
              val = rb_funcall(val, intern_localtime, 0);
            } else { // utc
              val = rb_funcall(val, intern_utc, 0);
            }
          }
          break;
        }
        case MYSQL_TYPE_TIMESTAMP:  // TIMESTAMP field
        case MYSQL_TYPE_DATETIME: { // DATETIME field
          unsigned int year, month, day, hour, min, sec, tokens;
          uint64_t seconds;

          tokens = sscanf(row[i], "%4d-%2d-%2d %2d:%2d:%2d", &year, &month, &day, &hour, &min, &sec);
          seconds = (year*31557600ULL) + (month*2592000ULL) + (day*86400ULL) + (hour*3600ULL) + (min*60ULL) + sec;

          if (seconds == 0) {
            val = Qnil;
          } else {
            if (month < 1 || day < 1) {
              rb_raise(cMysql2Error, "Invalid date: %s", row[i]);
              val = Qnil;
            } else {
              if (seconds < MYSQL2_MIN_TIME || seconds > MYSQL2_MAX_TIME) { // use DateTime instead
                VALUE offset = INT2NUM(0);
                if (db_timezone == intern_local) {
                  offset = rb_funcall(cMysql2Client, intern_local_offset, 0);
                }
                val = rb_funcall(cDateTime, intern_civil, 7, INT2NUM(year), INT2NUM(month), INT2NUM(day), INT2NUM(hour), INT2NUM(min), INT2NUM(sec), offset);
                if (!NIL_P(app_timezone)) {
                  if (app_timezone == intern_local) {
                    offset = rb_funcall(cMysql2Client, intern_local_offset, 0);
                    val = rb_funcall(val, intern_new_offset, 1, offset);
                  } else { // utc
                    val = rb_funcall(val, intern_new_offset, 1, opt_utc_offset);
                  }
                }
              } else {
                val = rb_funcall(rb_cTime, db_timezone, 6, INT2NUM(year), INT2NUM(month), INT2NUM(day), INT2NUM(hour), INT2NUM(min), INT2NUM(sec));
                if (!NIL_P(app_timezone)) {
                  if (app_timezone == intern_local) {
                    val = rb_funcall(val, intern_localtime, 0);
                  } else { // utc
                    val = rb_funcall(val, intern_utc, 0);
                  }
                }
              }
            }
          }
          break;
        }
        case MYSQL_TYPE_DATE:       // DATE field
        case MYSQL_TYPE_NEWDATE: {  // Newer const used > 5.0
          int year, month, day, tokens;
          tokens = sscanf(row[i], "%4d-%2d-%2d", &year, &month, &day);
          if (year+month+day == 0) {
            val = Qnil;
          } else {
            if (month < 1 || day < 1) {
              rb_raise(cMysql2Error, "Invalid date: %s", row[i]);
              val = Qnil;
            } else {
              val = rb_funcall(cDate, intern_new, 3, INT2NUM(year), INT2NUM(month), INT2NUM(day));
            }
          }
          break;
        }
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_STRING:     // CHAR or BINARY field
        case MYSQL_TYPE_SET:        // SET field
        case MYSQL_TYPE_ENUM:       // ENUM field
        case MYSQL_TYPE_GEOMETRY:   // Spatial fielda
        default:
          val = rb_str_new(row[i], fieldLengths[i]);
#ifdef HAVE_RUBY_ENCODING_H
          val = mysql2_set_field_string_encoding(val, fields[i], default_internal_enc, conn_enc);
#endif
          break;
        }
      }
      if (as == AS_HASH) {
        rb_hash_aset(rowVal, field, val);
      } else {
        rb_ary_push(rowVal, val);
      }
    } else {
      if (as == AS_HASH) {
        rb_hash_aset(rowVal, field, Qnil);
      } else {
        rb_ary_push(rowVal, Qnil);
      }
    }
  }

  /* TODO: raise exception if > 100 */
  if (as == AS_STRUCT) {
    if (wrapper->asStruct == Qnil) {
      char *buf[100];
      int num_fields;

      num_fields = wrapper->numberOfFields;
      if (num_fields > 100)
        num_fields = 100;

      for (i = 0; i < num_fields; i++) {
        MYSQL_FIELD *field = mysql_fetch_field_direct(wrapper->result, i);
        buf[i] = malloc(field->name_length + 1);
        memcpy(buf[i], field->name, field->name_length);
        buf[i][field->name_length] = 0;
      }
      wrapper->asStruct = rb_mysql_struct_define2(NULL, buf, num_fields);
      for (i = 0; i < num_fields; i++) {
        free(buf[i]);
      }
    }

    rowVal = rb_struct_alloc(wrapper->asStruct, rowVal);
  }

  return rowVal;
}

static VALUE rb_mysql_result_fetch_fields(VALUE self) {
  mysql2_result_wrapper * wrapper;
  unsigned int i = 0;
  short int symbolizeKeys = 0;
  VALUE defaults;

  GetMysql2Result(self, wrapper);

  defaults = rb_iv_get(self, "@query_options");
  if (rb_hash_aref(defaults, sym_symbolize_keys) == Qtrue) {
    symbolizeKeys = 1;
  }

  if (wrapper->fields == Qnil) {
    wrapper->numberOfFields = mysql_num_fields(wrapper->result);
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }

  if (RARRAY_LEN(wrapper->fields) != wrapper->numberOfFields) {
    for (i=0; i<wrapper->numberOfFields; i++) {
      rb_mysql_result_fetch_field(self, i, symbolizeKeys);
    }
  }

  return wrapper->fields;
}

static VALUE rb_mysql_result_each(int argc, VALUE * argv, VALUE self) {
  VALUE defaults, opts, block;
  ID db_timezone, app_timezone, dbTz, appTz;
  mysql2_result_wrapper * wrapper;
  unsigned long i;
  int symbolizeKeys = 0, as = AS_HASH, castBool = 0, cacheRows = 1, cast = 1, streaming = 0;
  MYSQL_FIELD * fields = NULL;

  GetMysql2Result(self, wrapper);

  defaults = rb_iv_get(self, "@query_options");
  if (rb_scan_args(argc, argv, "01&", &opts, &block) == 1) {
    opts = rb_funcall(defaults, intern_merge, 1, opts);
  } else {
    opts = defaults;
  }

  if (rb_hash_aref(opts, sym_symbolize_keys) == Qtrue) {
    symbolizeKeys = 1;
  }

  if (rb_hash_aref(opts, sym_as) == sym_array) {
    as = AS_ARRAY;
  } else if (rb_hash_aref(opts, sym_as) == sym_struct) {
    as = AS_STRUCT;
  }

  if (rb_hash_aref(opts, sym_cast_booleans) == Qtrue) {
    castBool = 1;
  }

  if (rb_hash_aref(opts, sym_cache_rows) == Qfalse) {
    cacheRows = 0;
  }

  if (rb_hash_aref(opts, sym_cast) == Qfalse) {
    cast = 0;
  }

  if(rb_hash_aref(opts, sym_stream) == Qtrue) {
    streaming = 1;
  }

  if(streaming && cacheRows) {
    rb_warn("cacheRows is ignored if streaming is true");
  }

  dbTz = rb_hash_aref(opts, sym_database_timezone);
  if (dbTz == sym_local) {
    db_timezone = intern_local;
  } else if (dbTz == sym_utc) {
    db_timezone = intern_utc;
  } else {
    if (!NIL_P(dbTz)) {
      rb_warn(":database_timezone option must be :utc or :local - defaulting to :local");
    }
    db_timezone = intern_local;
  }

  appTz = rb_hash_aref(opts, sym_application_timezone);
  if (appTz == sym_local) {
    app_timezone = intern_local;
  } else if (appTz == sym_utc) {
    app_timezone = intern_utc;
  } else {
    app_timezone = Qnil;
  }

  if (wrapper->lastRowProcessed == 0) {
    if(streaming) {
      // We can't get number of rows if we're streaming,
      // until we've finished fetching all rows
      wrapper->numberOfRows = 0;
      wrapper->rows = rb_ary_new();
    } else {
      wrapper->numberOfRows = mysql_num_rows(wrapper->result);
      if (wrapper->numberOfRows == 0) {
        wrapper->rows = rb_ary_new();
        return wrapper->rows;
      }
      wrapper->rows = rb_ary_new2(wrapper->numberOfRows);
    }
  }

  if (streaming) {
    if(!wrapper->streamingComplete) {
      VALUE row;

      fields = mysql_fetch_fields(wrapper->result);

      do {
        row = rb_mysql_result_fetch_row(self, db_timezone, app_timezone, symbolizeKeys, as, castBool, cast, fields);

        if (block != Qnil) {
          rb_yield(row);
          wrapper->lastRowProcessed++;
        }
      } while(row != Qnil);

      rb_mysql_result_free_result(wrapper);

      wrapper->numberOfRows = wrapper->lastRowProcessed;
      wrapper->streamingComplete = 1;
    } else {
      rb_raise(cMysql2Error, "You have already fetched all the rows for this query and streaming is true. (to reiterate you must requery).");
    }
  } else {
    if (cacheRows && wrapper->lastRowProcessed == wrapper->numberOfRows) {
      // we've already read the entire dataset from the C result into our
      // internal array. Lets hand that over to the user since it's ready to go
      for (i = 0; i < wrapper->numberOfRows; i++) {
        rb_yield(rb_ary_entry(wrapper->rows, i));
      }
    } else {
      unsigned long rowsProcessed = 0;
      rowsProcessed = RARRAY_LEN(wrapper->rows);
      fields = mysql_fetch_fields(wrapper->result);

      for (i = 0; i < wrapper->numberOfRows; i++) {
        VALUE row;
        if (cacheRows && i < rowsProcessed) {
          row = rb_ary_entry(wrapper->rows, i);
        } else {
          row = rb_mysql_result_fetch_row(self, db_timezone, app_timezone, symbolizeKeys, asArray, castBool, cast, fields);
          if (cacheRows) {
            rb_ary_store(wrapper->rows, i, row);
          }
          wrapper->lastRowProcessed++;
        }

        if (row == Qnil) {
          // we don't need the mysql C dataset around anymore, peace it
          rb_mysql_result_free_result(wrapper);
          return Qnil;
        }

        if (block != Qnil) {
          rb_yield(row);
        }
      }
      if (wrapper->lastRowProcessed == wrapper->numberOfRows) {
        // we don't need the mysql C dataset around anymore, peace it
        rb_mysql_result_free_result(wrapper);
      }
    }
  }

  return wrapper->rows;
}

static VALUE rb_mysql_result_count(VALUE self) {
  mysql2_result_wrapper *wrapper;

  GetMysql2Result(self, wrapper);
  if(wrapper->resultFreed) {
    return LONG2NUM(RARRAY_LEN(wrapper->rows));
  } else {
    return INT2FIX(mysql_num_rows(wrapper->result));
  }
}

/* Mysql2::Result */
VALUE rb_mysql_result_to_obj(MYSQL_RES * r) {
  VALUE obj;
  mysql2_result_wrapper * wrapper;
  obj = Data_Make_Struct(cMysql2Result, mysql2_result_wrapper, rb_mysql_result_mark, rb_mysql_result_free, wrapper);
  wrapper->numberOfFields = 0;
  wrapper->numberOfRows = 0;
  wrapper->lastRowProcessed = 0;
  wrapper->resultFreed = 0;
  wrapper->result = r;
  wrapper->fields = Qnil;
  wrapper->rows = Qnil;
  wrapper->encoding = Qnil;
  wrapper->asStruct = Qnil;
  wrapper->streamingComplete = 0;
  rb_obj_call_init(obj, 0, NULL);
  return obj;
}

void init_mysql2_result() {
  cBigDecimal = rb_const_get(rb_cObject, rb_intern("BigDecimal"));
  cDate = rb_const_get(rb_cObject, rb_intern("Date"));
  cDateTime = rb_const_get(rb_cObject, rb_intern("DateTime"));

  cMysql2Result = rb_define_class_under(mMysql2, "Result", rb_cObject);
  rb_define_method(cMysql2Result, "each", rb_mysql_result_each, -1);
  rb_define_method(cMysql2Result, "fields", rb_mysql_result_fetch_fields, 0);
  rb_define_method(cMysql2Result, "count", rb_mysql_result_count, 0);
  rb_define_alias(cMysql2Result, "size", "count");

  intern_encoding_from_charset = rb_intern("encoding_from_charset");
  intern_encoding_from_charset_code = rb_intern("encoding_from_charset_code");

  intern_new          = rb_intern("new");
  intern_utc          = rb_intern("utc");
  intern_local        = rb_intern("local");
  intern_merge        = rb_intern("merge");
  intern_localtime    = rb_intern("localtime");
  intern_local_offset = rb_intern("local_offset");
  intern_civil        = rb_intern("civil");
  intern_new_offset   = rb_intern("new_offset");

  sym_symbolize_keys  = ID2SYM(rb_intern("symbolize_keys"));
  sym_as              = ID2SYM(rb_intern("as"));
  sym_array           = ID2SYM(rb_intern("array"));
  sym_struct          = ID2SYM(rb_intern("struct"));
  sym_local           = ID2SYM(rb_intern("local"));
  sym_utc             = ID2SYM(rb_intern("utc"));
  sym_cast_booleans   = ID2SYM(rb_intern("cast_booleans"));
  sym_database_timezone     = ID2SYM(rb_intern("database_timezone"));
  sym_application_timezone  = ID2SYM(rb_intern("application_timezone"));
  sym_cache_rows     = ID2SYM(rb_intern("cache_rows"));
  sym_cast           = ID2SYM(rb_intern("cast"));
  sym_stream         = ID2SYM(rb_intern("stream"));

  opt_decimal_zero = rb_str_new2("0.0");
  rb_global_variable(&opt_decimal_zero); //never GC
  opt_float_zero = rb_float_new((double)0);
  rb_global_variable(&opt_float_zero);
  opt_time_year = INT2NUM(2000);
  opt_time_month = INT2NUM(1);
  opt_utc_offset = INT2NUM(0);

#ifdef HAVE_RUBY_ENCODING_H
  binaryEncoding = rb_enc_find("binary");
#endif
}

/*
# Ruby snippet to create the C function below.
File.open("bleh.c", "w") do |f|
    (1..100).each do |i|
        f.print("  case #{i}:\n")
        f.print("    st = rb_struct_define(name, ")
        i.times do |a|
            f.print("ary[#{a}], ")
        end
        f.print("NULL);\n")
        f.print("    break;\n")
    end
end
*/

/*
  Wrapper for intern.h:rb_struct_define().
  Takes array, len of the fields in the struct. Works around rb_struct_define()'s use of varargs.
  Stopping at 100 because fuck.
 */
static VALUE rb_mysql_struct_define2(const char *name, char **ary, int len) {
  VALUE st;

  switch (len) {
  case 1:
    st = rb_struct_define(name, ary[0], NULL);
    break;
  case 2:
    st = rb_struct_define(name, ary[0], ary[1], NULL);
    break;
  case 3:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], NULL);
    break;
  case 4:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], NULL);
    break;
  case 5:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], NULL);
    break;
  case 6:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], NULL);
    break;
  case 7:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], NULL);
    break;
  case 8:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], NULL);
    break;
  case 9:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[6], ary[8], NULL);
    break;
  case 10:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], NULL);
    break;  
  case 11:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], NULL);
    break;
  case 12:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], NULL);
    break;
  case 13:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], NULL);
    break;
  case 14:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], NULL);
    break;
  case 15:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], NULL);
    break;
  case 16:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], NULL);
    break;
  case 17:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], NULL);
    break;
  case 18:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], NULL);
    break;
  case 19:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], NULL);
    break;
  case 20:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], NULL);
    break;
  case 21:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], NULL);
    break;
  case 22:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], NULL);
    break;
  case 23:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], NULL);
    break;
  case 24:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], NULL);
    break;
  case 25:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], NULL);
    break;
  case 26:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], NULL);
    break;
  case 27:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], NULL);
    break;
  case 28:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], NULL);
    break;
  case 29:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], NULL);
    break;
  case 30:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], NULL);
    break;
  case 31:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], NULL);
    break;
  case 32:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], NULL);
    break;
  case 33:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], NULL);
    break;
  case 34:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], NULL);
    break;
  case 35:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], NULL);
    break;
  case 36:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], NULL);
    break;
  case 37:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], NULL);
    break;
  case 38:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], NULL);
    break;
  case 39:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], NULL);
    break;
  case 40:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], NULL);
    break;
  case 41:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], NULL);
    break;
  case 42:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], NULL);
    break;
  case 43:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], NULL);
    break;
  case 44:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], NULL);
    break;
  case 45:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], NULL);
    break;
  case 46:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], NULL);
    break;
  case 47:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], NULL);
    break;
  case 48:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], NULL);
    break;
  case 49:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], NULL);
    break;
  case 50:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], NULL);
    break;
  case 51:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], NULL);
    break;
  case 52:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], NULL);
    break;
  case 53:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], NULL);
    break;
  case 54:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], NULL);
    break;
  case 55:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], NULL);
    break;
  case 56:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], NULL);
    break;
  case 57:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], NULL);
    break;
  case 58:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], NULL);
    break;
  case 59:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], NULL);
    break;
  case 60:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], NULL);
    break;
  case 61:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], NULL);
    break;
  case 62:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], NULL);
    break;
  case 63:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], NULL);
    break;
  case 64:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], NULL);
    break;
  case 65:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], NULL);
    break;
  case 66:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], NULL);
    break;
  case 67:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], NULL);
    break;
  case 68:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], NULL);
    break;
  case 69:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], NULL);
    break;
  case 70:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], NULL);
    break;
  case 71:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], NULL);
    break;
  case 72:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], NULL);
    break;
  case 73:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], NULL);
    break;
  case 74:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], NULL);
    break;
  case 75:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], NULL);
    break;
  case 76:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], NULL);
    break;
  case 77:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], NULL);
    break;
  case 78:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], NULL);
    break;
  case 79:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], NULL);
    break;
  case 80:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], NULL);
    break;
  case 81:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], NULL);
    break;
  case 82:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], NULL);
    break;
  case 83:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], NULL);
    break;
  case 84:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], NULL);
    break;
  case 85:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], NULL);
    break;
  case 86:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], NULL);
    break;
  case 87:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], NULL);
    break;
  case 88:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], NULL);
    break;
  case 89:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], NULL);
    break;
  case 90:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], NULL);
    break;
  case 91:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], NULL);
    break;
  case 92:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], NULL);
    break;
  case 93:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], NULL);
    break;
  case 94:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], NULL);
    break;
  case 95:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], NULL);
    break;
  case 96:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], ary[95], NULL);
    break;
  case 97:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], ary[95], ary[96], NULL);
    break;
  case 98:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], ary[95], ary[96], ary[97], NULL);
    break;
  case 99:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], ary[95], ary[96], ary[97], ary[98], NULL);
    break;
  case 100:
    st = rb_struct_define(name, ary[0], ary[1], ary[2], ary[3], ary[4], ary[5], ary[6], ary[7], ary[8], ary[9], ary[10], ary[11], ary[12], ary[13], ary[14], ary[15], ary[16], ary[17], ary[18], ary[19], ary[20], ary[21], ary[22], ary[23], ary[24], ary[25], ary[26], ary[27], ary[28], ary[29], ary[30], ary[31], ary[32], ary[33], ary[34], ary[35], ary[36], ary[37], ary[38], ary[39], ary[40], ary[41], ary[42], ary[43], ary[44], ary[45], ary[46], ary[47], ary[48], ary[49], ary[50], ary[51], ary[52], ary[53], ary[54], ary[55], ary[56], ary[57], ary[58], ary[59], ary[60], ary[61], ary[62], ary[63], ary[64], ary[65], ary[66], ary[67], ary[68], ary[69], ary[70], ary[71], ary[72], ary[73], ary[74], ary[75], ary[76], ary[77], ary[78], ary[79], ary[80], ary[81], ary[82], ary[83], ary[84], ary[85], ary[86], ary[87], ary[88], ary[89], ary[90], ary[91], ary[92], ary[93], ary[94], ary[95], ary[96], ary[97], ary[98], ary[99], NULL);
    break;
  }

  return st;
}
