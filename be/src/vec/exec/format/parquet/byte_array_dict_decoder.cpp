// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/exec/format/parquet/byte_array_dict_decoder.h"

#include "util/coding.h"
#include "vec/columns/column_dictionary.h"
#include "vec/data_types/data_type_nullable.h"

namespace doris::vectorized {

Status ByteArrayDictDecoder::set_dict(std::unique_ptr<uint8_t[]>& dict, int32_t length,
                                      size_t num_values) {
    _dict = std::move(dict);
    _dict_items.reserve(num_values);
    uint32_t offset_cursor = 0;
    char* dict_item_address = reinterpret_cast<char*>(_dict.get());

    size_t total_length = 0;
    for (int i = 0; i < num_values; ++i) {
        uint32_t l = decode_fixed32_le(_dict.get() + offset_cursor);
        offset_cursor += 4;
        offset_cursor += l;
        total_length += l;
    }

    // For insert_many_strings_overflow
    _dict_data.resize(total_length + MAX_STRINGS_OVERFLOW_SIZE);
    _max_value_length = 0;
    size_t offset = 0;
    offset_cursor = 0;
    for (int i = 0; i < num_values; ++i) {
        uint32_t l = decode_fixed32_le(_dict.get() + offset_cursor);
        offset_cursor += 4;
        memcpy(&_dict_data[offset], dict_item_address + offset_cursor, l);
        _dict_items.emplace_back(&_dict_data[offset], l);
        offset_cursor += l;
        offset += l;
        if (offset_cursor > length) {
            return Status::Corruption("Wrong data length in dictionary");
        }
        if (l > _max_value_length) {
            _max_value_length = l;
        }
    }
    if (offset_cursor != length) {
        return Status::Corruption("Wrong dictionary data for byte array type");
    }
    return Status::OK();
}

Status ByteArrayDictDecoder::decode_values(MutableColumnPtr& doris_column, DataTypePtr& data_type,
                                           ColumnSelectVector& select_vector) {
    size_t non_null_size = select_vector.num_values() - select_vector.num_nulls();
    if (doris_column->is_column_dictionary() &&
        assert_cast<ColumnDictI32&>(*doris_column).dict_size() == 0) {
        assert_cast<ColumnDictI32&>(*doris_column)
                .insert_many_dict_data(&_dict_items[0], _dict_items.size());
    }
    _indexes.resize(non_null_size);
    _index_batch_decoder->GetBatch(&_indexes[0], non_null_size);

    if (doris_column->is_column_dictionary()) {
        return _decode_dict_values(doris_column, select_vector);
    }

    TypeIndex logical_type = remove_nullable(data_type)->get_type_id();
    switch (logical_type) {
    case TypeIndex::String:
    case TypeIndex::FixedString: {
        size_t dict_index = 0;

        ColumnSelectVector::DataReadType read_type;
        while (size_t run_length = select_vector.get_next_run(&read_type)) {
            switch (read_type) {
            case ColumnSelectVector::CONTENT: {
                std::vector<StringRef> string_values;
                string_values.reserve(run_length);
                for (size_t i = 0; i < run_length; ++i) {
                    string_values.emplace_back(_dict_items[_indexes[dict_index++]]);
                }
                doris_column->insert_many_strings_overflow(&string_values[0], run_length,
                                                           _max_value_length);
                break;
            }
            case ColumnSelectVector::NULL_DATA: {
                doris_column->insert_many_defaults(run_length);
                break;
            }
            case ColumnSelectVector::FILTERED_CONTENT: {
                dict_index += run_length;
                break;
            }
            case ColumnSelectVector::FILTERED_NULL: {
                // do nothing
                break;
            }
            }
        }
        return Status::OK();
    }
    case TypeIndex::Decimal32:
        return _decode_binary_decimal<Int32>(doris_column, data_type, select_vector);
    case TypeIndex::Decimal64:
        return _decode_binary_decimal<Int64>(doris_column, data_type, select_vector);
    case TypeIndex::Decimal128:
        return _decode_binary_decimal<Int128>(doris_column, data_type, select_vector);
    case TypeIndex::Decimal128I:
        return _decode_binary_decimal<Int128>(doris_column, data_type, select_vector);
    default:
        break;
    }
    return Status::InvalidArgument(
            "Can't decode parquet physical type BYTE_ARRAY to doris logical type {}",
            getTypeName(logical_type));
}
} // namespace doris::vectorized