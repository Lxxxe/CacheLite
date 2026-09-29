#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cachelite::protocol {

struct RespValue {
    enum class Type {
        SimpleString,   //+OK\r\n简单字符串
        Error,          //-ERR message\r\n错误信息
        Integer,        //:100\r\n整数
        BulkString,     //$5\r\nhello\r\n批量字符串
        Array,          //*2\r\n...数组
        NullBulkString, //$-1\r\n空字符串
        NullArray       //*-1\r\n空数组
    };

    //实际只有一个字段组会有效：
    Type type{Type::SimpleString};//记录当前对象的 RESP 类型。默认SimpleString
    std::string text;//用于保存字符串类型的数据,包括Simple String、Error、Bulk String
    std::int64_t integer{0};//用于保存 RESP 整数
    std::vector<RespValue> elements;//用于保存 RESP 数组中的多个元素

    //工厂函数,统一创建不同类型的 RespValue
    //这些函数都是 static，调用时不需要先创建对象：
    [[nodiscard]] static RespValue simpleString(std::string value);
    [[nodiscard]] static RespValue error(std::string value);
    [[nodiscard]] static RespValue integerValue(std::int64_t value);
    [[nodiscard]] static RespValue bulkString(std::string value);//创建批量字符串
    [[nodiscard]] static RespValue array(std::vector<RespValue> values);//创建 RESP 数组
    [[nodiscard]] static RespValue nullBulkString();//创建空 Bulk String
    [[nodiscard]] static RespValue nullArray();//创建空数组
};

}  // namespace cachelite::protocol
