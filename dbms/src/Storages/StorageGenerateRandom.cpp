#include <Storages/IStorage.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/StorageGenerateRandom.h>
#include <Storages/StorageFactory.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <Processors/Pipe.h>
#include <Parsers/ASTLiteral.h>

#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeDecimalBase.h>
#include <DataTypes/DataTypeArray.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnTuple.h>

#include <Common/SipHash.h>
#include <Common/randomSeed.h>
#include <common/unaligned.h>

#include <Functions/FunctionFactory.h>

#include <pcg_random.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}


namespace
{

void fillBufferWithRandomData(char * __restrict data, size_t size, pcg64_fast & rng)
{
    char * __restrict end = data + size;
    while (data < end)
    {
        /// The loop can be further optimized.
        UInt64 number = rng();
        unalignedStore<UInt64>(data, number);
        data += sizeof(UInt64); /// We assume that data has 15-byte padding (see PaddedPODArray)
    }
}


ColumnPtr fillColumnWithRandomData(
    const DataTypePtr type, UInt64 limit, UInt64 max_array_length, UInt64 max_string_length, pcg64_fast & rng, const Context & context)
{
    TypeIndex idx = type->getTypeId();

    switch (idx)
    {
        case TypeIndex::String:
        {
            Block block{ColumnWithTypeAndName{nullptr, type, "result"}};
            FunctionFactory::instance().get("randomPrintableASCII", context)->build({})->execute(block, {}, 0, limit);
            return block.getByPosition(0).column;
        }

        case TypeIndex::Enum8:
        {
            auto column = ColumnVector<Int8>::create();
            auto values = typeid_cast<const DataTypeEnum<Int8> *>(type.get())->getValues();
            auto & data = column->getData();
            data.resize(limit);

            UInt8 size = values.size();
            UInt8 off;
            for (UInt64 i = 0; i < limit; ++i)
            {
                off = static_cast<UInt8>(rng()) % size;
                data[i] = values[off].second;
            }

            return column;
        }

        case TypeIndex::Enum16:
        {
            auto column = ColumnVector<Int16>::create();
            auto values = typeid_cast<const DataTypeEnum<Int16> *>(type.get())->getValues();
            auto & data = column->getData();
            data.resize(limit);

            UInt16 size = values.size();
            UInt8 off;
            for (UInt64 i = 0; i < limit; ++i)
            {
                off = static_cast<UInt16>(rng()) % size;
                data[i] = values[off].second;
            }

            return column;
        }

        case TypeIndex::Array:
        {
            auto nested_type = typeid_cast<const DataTypeArray *>(type.get())->getNestedType();

            auto offsets_column = ColumnVector<ColumnArray::Offset>::create();
            auto & offsets = offsets_column->getData();

            UInt64 offset = 0;
            offsets.resize(limit);
            for (UInt64 i = 0; i < limit; ++i)
            {
                offset += static_cast<UInt64>(rng()) % max_array_length;
                offsets[i] = offset;
            }

            auto data_column = fillColumnWithRandomData(nested_type, offset, max_array_length, max_string_length, rng, context);

            return ColumnArray::create(std::move(data_column), std::move(offsets_column));
        }

        case TypeIndex::Tuple:
        {
            auto elements = typeid_cast<const DataTypeTuple *>(type.get())->getElements();
            const size_t tuple_size = elements.size();
            Columns tuple_columns(tuple_size);

            for (size_t i = 0; i < tuple_size; ++i)
                tuple_columns[i] = fillColumnWithRandomData(elements[i], limit, max_array_length, max_string_length, rng, context);

            return ColumnTuple::create(std::move(tuple_columns));
        }

        case TypeIndex::Nullable:
        {
            auto nested_type = typeid_cast<const DataTypeNullable *>(type.get())->getNestedType();
            auto nested_column = fillColumnWithRandomData(nested_type, limit, max_array_length, max_string_length, rng, context);

            auto null_map_column = ColumnUInt8::create();
            auto & null_map = null_map_column->getData();
            null_map.resize(limit);
            for (UInt64 i = 0; i < limit; ++i)
                null_map[i] = rng() % 16 == 0; /// No real motivation for this.

            return ColumnNullable::create(std::move(nested_column), std::move(null_map_column));
        }

        case TypeIndex::UInt8:
        {
            auto column = ColumnUInt8::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt8), rng);
            return column;
        }
        case TypeIndex::UInt16: [[fallthrough]];
        case TypeIndex::Date:
        {
            auto column = ColumnUInt16::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt16), rng);
            return column;
        }
        case TypeIndex::UInt32: [[fallthrough]];
        case TypeIndex::DateTime:
        {
            auto column = ColumnUInt32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt32), rng);
            return column;
        }
        case TypeIndex::UInt64:
        {
            auto column = ColumnUInt64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt64), rng);
            return column;
        }
        case TypeIndex::UInt128: [[fallthrough]];
        case TypeIndex::UUID:
        {
            auto column = ColumnUInt128::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(UInt128), rng);
            return column;
        }
        case TypeIndex::Int8:
        {
            auto column = ColumnInt8::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int8), rng);
            return column;
        }
        case TypeIndex::Int16:
        {
            auto column = ColumnInt16::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int16), rng);
            return column;
        }
        case TypeIndex::Int32:
        {
            auto column = ColumnInt32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int32), rng);
            return column;
        }
        case TypeIndex::Int64:
        {
            auto column = ColumnInt64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Int64), rng);
            return column;
        }
        case TypeIndex::Float32:
        {
            auto column = ColumnFloat32::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Float32), rng);
            return column;
        }
        case TypeIndex::Float64:
        {
            auto column = ColumnFloat64::create();
            column->getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column->getData().data()), limit * sizeof(Float64), rng);
            return column;
        }
        case TypeIndex::Decimal32:
        {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal32> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal32), rng);
            return column;
        }
        case TypeIndex::Decimal64: [[fallthrough]];
        case TypeIndex::DateTime64:
        {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal64> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal64), rng);
            return column;
        }
        case TypeIndex::Decimal128:
        {
            auto column = type->createColumn();
            auto & column_concrete = typeid_cast<ColumnDecimal<Decimal128> &>(*column);
            column_concrete.getData().resize(limit);
            fillBufferWithRandomData(reinterpret_cast<char *>(column_concrete.getData().data()), limit * sizeof(Decimal128), rng);
            return column;
        }

        default:
            throw Exception("The 'GenerateRandom' is not implemented for type " + type->getName(), ErrorCodes::NOT_IMPLEMENTED);
    }
}


class GenerateSource : public SourceWithProgress
{
public:
    GenerateSource(UInt64 block_size_, UInt64 max_array_length_, UInt64 max_string_length_, UInt64 random_seed_, Block block_header_, const Context & context_)
        : SourceWithProgress(block_header_), block_size(block_size_), max_array_length(max_array_length_), max_string_length(max_string_length_)
        , block_header(block_header_), rng(random_seed_), context(context_) {}

    String getName() const override { return "GenerateRandom"; }

protected:
    Chunk generate() override
    {
        Columns columns;
        columns.reserve(block_header.columns());
        DataTypes types = block_header.getDataTypes();

        for (const auto & type : types)
            columns.emplace_back(fillColumnWithRandomData(type, block_size, max_array_length, max_string_length, rng, context));

        return {std::move(columns), block_size};
    }

private:
    UInt64 block_size;
    UInt64 max_array_length;
    UInt64 max_string_length;
    Block block_header;

    pcg64_fast rng;

    const Context & context;
};

}


StorageGenerateRandom::StorageGenerateRandom(const StorageID & table_id_, const ColumnsDescription & columns_,
    UInt64 max_array_length_, UInt64 max_string_length_, UInt64 random_seed_)
    : IStorage(table_id_), max_array_length(max_array_length_), max_string_length(max_string_length_)
{
    random_seed = random_seed_ ? random_seed_ : randomSeed();
    setColumns(columns_);
}


void registerStorageGenerateRandom(StorageFactory & factory)
{
    factory.registerStorage("GenerateRandom", [](const StorageFactory::Arguments & args)
    {
        ASTs & engine_args = args.engine_args;

        if (engine_args.size() > 3)
            throw Exception("Storage GenerateRandom requires at most three arguments: "\
                        "max_array_length, max_string_length, random_seed.",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        UInt64 max_array_length_ = 10;
        UInt64 max_string_length_ = 10;
        UInt64 random_seed_ = 0; // zero for random

        /// Parsing second argument if present
        if (engine_args.size() >= 1)
            max_array_length_ = engine_args[0]->as<ASTLiteral &>().value.safeGet<UInt64>();

        if (engine_args.size() >= 2)
            max_string_length_ = engine_args[1]->as<ASTLiteral &>().value.safeGet<UInt64>();

        if (engine_args.size() == 3)
            random_seed_ = engine_args[2]->as<ASTLiteral &>().value.safeGet<UInt64>();

        return StorageGenerateRandom::create(args.table_id, args.columns, max_array_length_, max_string_length_, random_seed_);
    });
}

Pipes StorageGenerateRandom::read(
    const Names & column_names,
    const SelectQueryInfo & /*query_info*/,
    const Context & context,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    unsigned num_streams)
{
    check(column_names, true);

    Pipes pipes;
    pipes.reserve(num_streams);

    const ColumnsDescription & columns_ = getColumns();
    Block block_header;
    for (const auto & name : column_names)
    {
        const auto & name_type = columns_.get(name);
        MutableColumnPtr column = name_type.type->createColumn();
        block_header.insert({std::move(column), name_type.type, name_type.name});
    }

    /// Will create more seed values for each source from initial seed.
    pcg64_fast generate(random_seed);

    for (UInt64 i = 0; i < num_streams; ++i)
        pipes.emplace_back(std::make_shared<GenerateSource>(max_block_size, max_array_length, max_string_length, generate(), block_header, context));

    return pipes;
}

}
