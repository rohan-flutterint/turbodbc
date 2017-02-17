#include <turbodbc_arrow/arrow_result_set.h>

// Somewhere a macro defines BOOL as a constant. This is in conflict with array/type.h
#undef BOOL
#include <arrow/array.h>
#include <arrow/builder.h>
#include <arrow/column.h>
#include <arrow/schema.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/memory_pool.h>

#include <pyarrow/table_api.h>

#include <sql.h>

#include <vector>

using arrow::default_memory_pool;
using arrow::ArrayBuilder;
using arrow::BooleanBuilder;
using arrow::DateBuilder;
using arrow::DoubleBuilder;
using arrow::Int64Builder;
using arrow::Status;
using arrow::StringBuilder;
using arrow::TimeUnit;
using arrow::TimestampBuilder;

namespace turbodbc_arrow {


namespace {

	std::unique_ptr<ArrayBuilder> make_array_builder(turbodbc::type_code type)
	{
		switch (type) {
			case turbodbc::type_code::floating_point:
				return std::unique_ptr<ArrayBuilder>(new DoubleBuilder(default_memory_pool(), arrow::float64()));
			case turbodbc::type_code::integer:
				return std::unique_ptr<ArrayBuilder>(new Int64Builder(default_memory_pool(), arrow::int64()));
			case turbodbc::type_code::boolean:
				return std::unique_ptr<ArrayBuilder>(new BooleanBuilder(default_memory_pool(), std::make_shared<arrow::BooleanType>()));
			case turbodbc::type_code::timestamp:
				return std::unique_ptr<TimestampBuilder>(new TimestampBuilder(default_memory_pool(), arrow::timestamp(TimeUnit::MICRO)));
			case turbodbc::type_code::date:
        return std::unique_ptr<DateBuilder>(new DateBuilder(default_memory_pool(), arrow::date()));
			default:
				return std::unique_ptr<StringBuilder>(new StringBuilder(default_memory_pool()));
		}
	}

  // Copied from turbodbc_numpy/src/datetime_column.cpp
  boost::posix_time::ptime const timestamp_epoch({1970, 1, 1}, {0, 0, 0, 0});

	int64_t timestamp_to_microseconds(char const * data_pointer)
	{
		auto & sql_ts = *reinterpret_cast<SQL_TIMESTAMP_STRUCT const *>(data_pointer);
		long const microseconds = sql_ts.fraction / 1000;
    struct tm datetime = {0};
    datetime.tm_year = sql_ts.year - 1900;
    datetime.tm_mon = sql_ts.month - 1;
    datetime.tm_mday = sql_ts.day;
    datetime.tm_hour = sql_ts.hour;
    datetime.tm_min = sql_ts.minute;
    datetime.tm_sec = sql_ts.second;
    struct tm epoch = {0};
    epoch.tm_year = 70;
    epoch.tm_mday = 1;
    
    // Microseconds since the epoch
    return lrint(difftime(mktime(&datetime), mktime(&epoch))) * 1000000 + microseconds;
	}

	int64_t date_to_days(char const * data_pointer)
	{
		auto & sql_date = *reinterpret_cast<SQL_DATE_STRUCT const *>(data_pointer);
    
    struct tm date = {0};
    date.tm_year = sql_date.year - 1900;
    date.tm_mon = sql_date.month - 1;
    date.tm_mday = sql_date.day;
    struct tm epoch = {0};
    epoch.tm_year = 70;
    epoch.tm_mday = 1;
    
    // Milliseconds since the epoch
    return lrint(difftime(mktime(&date), mktime(&epoch)) * 1000);
	}
}

arrow_result_set::arrow_result_set(turbodbc::result_sets::result_set & base) :
	base_result_(base)
{
}

std::shared_ptr<arrow::Schema> arrow_result_set::schema()
{
	auto const column_info = base_result_.get_column_info();
  auto const n_columns = column_info.size();
  std::vector<std::shared_ptr<arrow::Field>> fields;
	for (std::size_t i = 0; i != n_columns; ++i) {
    std::shared_ptr<arrow::DataType> type;
		switch (column_info[i].type) {
			case turbodbc::type_code::floating_point:
        type = arrow::float64();
        break;
			case turbodbc::type_code::integer:
        type = arrow::int64();
        break;
			case turbodbc::type_code::boolean:
        type = arrow::boolean();
        break;
			case turbodbc::type_code::timestamp:
        type = arrow::timestamp(TimeUnit::MICRO);
        break;
			case turbodbc::type_code::date:
        type = arrow::date();
        break;
			default:
        type = std::make_shared<arrow::StringType>();
		}
    fields.emplace_back(std::make_shared<arrow::Field>(column_info[i].name, type, column_info[i].supports_null_values));
	}
  return std::make_shared<arrow::Schema>(fields);
}

Status append_to_double_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t* valid_bytes) {
  auto typed_builder = static_cast<DoubleBuilder*>(builder.get());
  auto data_ptr = reinterpret_cast<const double*>(input_buffer.data_pointer());
  return typed_builder->Append(data_ptr, rows_in_batch, valid_bytes);
}

Status append_to_int_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t* valid_bytes) {
  auto typed_builder = static_cast<Int64Builder*>(builder.get());
  auto data_ptr = reinterpret_cast<const int64_t*>(input_buffer.data_pointer());
  return typed_builder->Append(data_ptr, rows_in_batch, valid_bytes);
}

Status append_to_bool_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t* valid_bytes) {
  auto typed_builder = static_cast<BooleanBuilder*>(builder.get());
  auto data_ptr = reinterpret_cast<const uint8_t*>(input_buffer.data_pointer());
  return typed_builder->Append(data_ptr, rows_in_batch, valid_bytes);
}

Status append_to_timestamp_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t*) {
  auto typed_builder = static_cast<TimestampBuilder*>(builder.get());
  for (std::size_t j = 0; j < rows_in_batch; ++j) {
    auto element = input_buffer[j];
  	if (element.indicator == SQL_NULL_DATA) {
      ARROW_RETURN_NOT_OK(typed_builder->AppendNull());
  	} else {
  		ARROW_RETURN_NOT_OK(typed_builder->Append(timestamp_to_microseconds(element.data_pointer)));
  	}
  }
  return Status::OK();
}

Status append_to_date_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t*) {
  auto typed_builder = static_cast<DateBuilder*>(builder.get());
  for (std::size_t j = 0; j < rows_in_batch; ++j) {
    auto element = input_buffer[j];
  	if (element.indicator == SQL_NULL_DATA) {
      ARROW_RETURN_NOT_OK(typed_builder->AppendNull());
  	} else {
  		ARROW_RETURN_NOT_OK(typed_builder->Append(date_to_days(element.data_pointer)));
  	}
  }
  return Status::OK();
}

Status append_to_string_builder(size_t rows_in_batch, const std::unique_ptr<ArrayBuilder>& builder, const cpp_odbc::multi_value_buffer& input_buffer, uint8_t*) {
  auto typed_builder = static_cast<StringBuilder*>(builder.get());
  for (std::size_t j = 0; j != rows_in_batch; ++j) {
  	auto const element = input_buffer[j];
  	if (element.indicator == SQL_NULL_DATA) {
      ARROW_RETURN_NOT_OK(typed_builder->AppendNull());
  	} else {
      ARROW_RETURN_NOT_OK(typed_builder->Append(element.data_pointer, element.indicator));
  	}
  }
  return Status::OK();
}

Status arrow_result_set::fetch_all_native(std::shared_ptr<arrow::Table>* out)
{
	std::size_t rows_in_batch = base_result_.fetch_next_batch();
	auto const column_info = base_result_.get_column_info();
  auto const n_columns = column_info.size();

  // Construct the Arrow schema from the SQL schema information
  std::shared_ptr<arrow::Schema> arrow_schema = schema();

  // Create Builders for all columns
	std::vector<std::unique_ptr<ArrayBuilder>> columns;
	for (std::size_t i = 0; i != n_columns; ++i) {
		columns.push_back(make_array_builder(column_info[i].type));
	}

	do {
    std::vector<std::reference_wrapper<cpp_odbc::multi_value_buffer const>> const buffers = base_result_.get_buffers();

    // TODO: Use a PoolBuffer for this and only allocate it once
    std::vector<uint8_t> valid_bytes(rows_in_batch);
		for (size_t i = 0; i != n_columns; ++i) {
	    auto const indicator_pointer = buffers[i].get().indicator_pointer();
      for (size_t element = 0; element != rows_in_batch; ++element) {
        if (indicator_pointer[element] == SQL_NULL_DATA) {
          valid_bytes[element] = 0;
        } else {
          valid_bytes[element] = 1;
        }
      }
	  	switch (column_info[i].type) {
	  		case turbodbc::type_code::floating_point:
          ARROW_RETURN_NOT_OK(append_to_double_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  		case turbodbc::type_code::integer:
          ARROW_RETURN_NOT_OK(append_to_int_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  		case turbodbc::type_code::boolean:
          ARROW_RETURN_NOT_OK(append_to_bool_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  		case turbodbc::type_code::timestamp:
          ARROW_RETURN_NOT_OK(append_to_timestamp_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  		case turbodbc::type_code::date:
          ARROW_RETURN_NOT_OK(append_to_date_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  		default:
          // Strings are the only remaining type
          ARROW_RETURN_NOT_OK(append_to_string_builder(rows_in_batch, columns[i], buffers[i].get(), valid_bytes.data()));
          break;
	  	}
		}
		rows_in_batch = base_result_.fetch_next_batch();
	} while (rows_in_batch != 0);

  // TODO(ARROW-XXX): Add convenience constructor for Table from Schema+Arrays
  std::vector<std::shared_ptr<arrow::Column>> arrow_columns;
  for (size_t i = 0; i != n_columns; ++i) {
    std::shared_ptr<arrow::Array> array;
    columns[i]->Finish(&array);
    arrow_columns.emplace_back(std::make_shared<arrow::Column>(
          arrow_schema->field(i), array));
  }

  *out = std::make_shared<arrow::Table>("", arrow_schema, arrow_columns);
  return Status::OK();
}

pybind11::object arrow_result_set::fetch_all()
{
  std::shared_ptr<arrow::Table> table;
  fetch_all_native(&table);
  import_pyarrow__table();
	return pybind11::object(pybind11::handle(__pyx_api_f_7pyarrow_5table_table_from_ctable(table)), true);
}


}
