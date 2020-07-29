// automatically generated by the FlatBuffers compiler, do not modify


#ifndef FLATBUFFERS_GENERATED_MULTIROWFLATBUFFER_TABLES_H_
#define FLATBUFFERS_GENERATED_MULTIROWFLATBUFFER_TABLES_H_

#include "flatbuffers/flatbuffers.h"

namespace Tables {

struct Date;

struct Table;

struct Row;

FLATBUFFERS_MANUALLY_ALIGNED_STRUCT(4) Date FLATBUFFERS_FINAL_CLASS {
 private:
  int32_t year_;
  int32_t month_;
  int32_t day_;

 public:
  Date() {
    memset(this, 0, sizeof(Date));
  }
  Date(int32_t _year, int32_t _month, int32_t _day)
      : year_(flatbuffers::EndianScalar(_year)),
        month_(flatbuffers::EndianScalar(_month)),
        day_(flatbuffers::EndianScalar(_day)) {
  }
  int32_t year() const {
    return flatbuffers::EndianScalar(year_);
  }
  int32_t month() const {
    return flatbuffers::EndianScalar(month_);
  }
  int32_t day() const {
    return flatbuffers::EndianScalar(day_);
  }
};
FLATBUFFERS_STRUCT_END(Date, 12);

struct Table FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  enum {
    VT_ROWS = 4
  };
  const flatbuffers::Vector<flatbuffers::Offset<Row>> *rows() const {
    return GetPointer<const flatbuffers::Vector<flatbuffers::Offset<Row>> *>(VT_ROWS);
  }
  bool Verify(flatbuffers::Verifier &verifier) const {
    return VerifyTableStart(verifier) &&
           VerifyOffset(verifier, VT_ROWS) &&
           verifier.Verify(rows()) &&
           verifier.VerifyVectorOfTables(rows()) &&
           verifier.EndTable();
  }
};

struct TableBuilder {
  flatbuffers::FlatBufferBuilder &fbb_;
  flatbuffers::uoffset_t start_;
  void add_rows(flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Row>>> rows) {
    fbb_.AddOffset(Table::VT_ROWS, rows);
  }
  explicit TableBuilder(flatbuffers::FlatBufferBuilder &_fbb)
        : fbb_(_fbb) {
    start_ = fbb_.StartTable();
  }
  TableBuilder &operator=(const TableBuilder &);
  flatbuffers::Offset<Table> Finish() {
    const auto end = fbb_.EndTable(start_);
    auto o = flatbuffers::Offset<Table>(end);
    return o;
  }
};

inline flatbuffers::Offset<Table> CreateTable(
    flatbuffers::FlatBufferBuilder &_fbb,
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Row>>> rows = 0) {
  TableBuilder builder_(_fbb);
  builder_.add_rows(rows);
  return builder_.Finish();
}

inline flatbuffers::Offset<Table> CreateTableDirect(
    flatbuffers::FlatBufferBuilder &_fbb,
    const std::vector<flatbuffers::Offset<Row>> *rows = nullptr) {
  return Tables::CreateTable(
      _fbb,
      rows ? _fbb.CreateVector<flatbuffers::Offset<Row>>(*rows) : 0);
}

struct Row FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  enum {
    VT_L_ORDERKEY = 4,
    VT_L_PARTKEY = 6,
    VT_L_SUPPKEY = 8,
    VT_L_LINENUMBER = 10,
    VT_L_QUANTITY = 12,
    VT_L_EXTENDEDPRICE = 14,
    VT_L_DISCOUNT = 16,
    VT_L_TAX = 18,
    VT_L_RETURNFLAG = 20,
    VT_L_LINESTATUS = 22,
    VT_L_SHIPDATE = 24,
    VT_L_RECEIPTDATE = 26,
    VT_L_COMMITDATE = 28,
    VT_L_SHIPINSTRUCT = 30,
    VT_L_SHIPMODE = 32,
    VT_L_COMMENT = 34
  };
  int32_t L_ORDERKEY() const {
    return GetField<int32_t>(VT_L_ORDERKEY, 0);
  }
  int32_t L_PARTKEY() const {
    return GetField<int32_t>(VT_L_PARTKEY, 0);
  }
  int32_t L_SUPPKEY() const {
    return GetField<int32_t>(VT_L_SUPPKEY, 0);
  }
  int32_t L_LINENUMBER() const {
    return GetField<int32_t>(VT_L_LINENUMBER, 0);
  }
  float L_QUANTITY() const {
    return GetField<float>(VT_L_QUANTITY, 0.0f);
  }
  float L_EXTENDEDPRICE() const {
    return GetField<float>(VT_L_EXTENDEDPRICE, 0.0f);
  }
  float L_DISCOUNT() const {
    return GetField<float>(VT_L_DISCOUNT, 0.0f);
  }
  float L_TAX() const {
    return GetField<float>(VT_L_TAX, 0.0f);
  }
  int32_t L_RETURNFLAG() const {
    return GetField<int32_t>(VT_L_RETURNFLAG, 0);
  }
  int32_t L_LINESTATUS() const {
    return GetField<int32_t>(VT_L_LINESTATUS, 0);
  }
  const Date *L_SHIPDATE() const {
    return GetStruct<const Date *>(VT_L_SHIPDATE);
  }
  const Date *L_RECEIPTDATE() const {
    return GetStruct<const Date *>(VT_L_RECEIPTDATE);
  }
  const Date *L_COMMITDATE() const {
    return GetStruct<const Date *>(VT_L_COMMITDATE);
  }
  const flatbuffers::String *L_SHIPINSTRUCT() const {
    return GetPointer<const flatbuffers::String *>(VT_L_SHIPINSTRUCT);
  }
  const flatbuffers::String *L_SHIPMODE() const {
    return GetPointer<const flatbuffers::String *>(VT_L_SHIPMODE);
  }
  const flatbuffers::String *L_COMMENT() const {
    return GetPointer<const flatbuffers::String *>(VT_L_COMMENT);
  }
  bool Verify(flatbuffers::Verifier &verifier) const {
    return VerifyTableStart(verifier) &&
           VerifyField<int32_t>(verifier, VT_L_ORDERKEY) &&
           VerifyField<int32_t>(verifier, VT_L_PARTKEY) &&
           VerifyField<int32_t>(verifier, VT_L_SUPPKEY) &&
           VerifyField<int32_t>(verifier, VT_L_LINENUMBER) &&
           VerifyField<float>(verifier, VT_L_QUANTITY) &&
           VerifyField<float>(verifier, VT_L_EXTENDEDPRICE) &&
           VerifyField<float>(verifier, VT_L_DISCOUNT) &&
           VerifyField<float>(verifier, VT_L_TAX) &&
           VerifyField<int32_t>(verifier, VT_L_RETURNFLAG) &&
           VerifyField<int32_t>(verifier, VT_L_LINESTATUS) &&
           VerifyField<Date>(verifier, VT_L_SHIPDATE) &&
           VerifyField<Date>(verifier, VT_L_RECEIPTDATE) &&
           VerifyField<Date>(verifier, VT_L_COMMITDATE) &&
           VerifyOffset(verifier, VT_L_SHIPINSTRUCT) &&
           verifier.Verify(L_SHIPINSTRUCT()) &&
           VerifyOffset(verifier, VT_L_SHIPMODE) &&
           verifier.Verify(L_SHIPMODE()) &&
           VerifyOffset(verifier, VT_L_COMMENT) &&
           verifier.Verify(L_COMMENT()) &&
           verifier.EndTable();
  }
};

struct RowBuilder {
  flatbuffers::FlatBufferBuilder &fbb_;
  flatbuffers::uoffset_t start_;
  void add_L_ORDERKEY(int32_t L_ORDERKEY) {
    fbb_.AddElement<int32_t>(Row::VT_L_ORDERKEY, L_ORDERKEY, 0);
  }
  void add_L_PARTKEY(int32_t L_PARTKEY) {
    fbb_.AddElement<int32_t>(Row::VT_L_PARTKEY, L_PARTKEY, 0);
  }
  void add_L_SUPPKEY(int32_t L_SUPPKEY) {
    fbb_.AddElement<int32_t>(Row::VT_L_SUPPKEY, L_SUPPKEY, 0);
  }
  void add_L_LINENUMBER(int32_t L_LINENUMBER) {
    fbb_.AddElement<int32_t>(Row::VT_L_LINENUMBER, L_LINENUMBER, 0);
  }
  void add_L_QUANTITY(float L_QUANTITY) {
    fbb_.AddElement<float>(Row::VT_L_QUANTITY, L_QUANTITY, 0.0f);
  }
  void add_L_EXTENDEDPRICE(float L_EXTENDEDPRICE) {
    fbb_.AddElement<float>(Row::VT_L_EXTENDEDPRICE, L_EXTENDEDPRICE, 0.0f);
  }
  void add_L_DISCOUNT(float L_DISCOUNT) {
    fbb_.AddElement<float>(Row::VT_L_DISCOUNT, L_DISCOUNT, 0.0f);
  }
  void add_L_TAX(float L_TAX) {
    fbb_.AddElement<float>(Row::VT_L_TAX, L_TAX, 0.0f);
  }
  void add_L_RETURNFLAG(int32_t L_RETURNFLAG) {
    fbb_.AddElement<int32_t>(Row::VT_L_RETURNFLAG, L_RETURNFLAG, 0);
  }
  void add_L_LINESTATUS(int32_t L_LINESTATUS) {
    fbb_.AddElement<int32_t>(Row::VT_L_LINESTATUS, L_LINESTATUS, 0);
  }
  void add_L_SHIPDATE(const Date *L_SHIPDATE) {
    fbb_.AddStruct(Row::VT_L_SHIPDATE, L_SHIPDATE);
  }
  void add_L_RECEIPTDATE(const Date *L_RECEIPTDATE) {
    fbb_.AddStruct(Row::VT_L_RECEIPTDATE, L_RECEIPTDATE);
  }
  void add_L_COMMITDATE(const Date *L_COMMITDATE) {
    fbb_.AddStruct(Row::VT_L_COMMITDATE, L_COMMITDATE);
  }
  void add_L_SHIPINSTRUCT(flatbuffers::Offset<flatbuffers::String> L_SHIPINSTRUCT) {
    fbb_.AddOffset(Row::VT_L_SHIPINSTRUCT, L_SHIPINSTRUCT);
  }
  void add_L_SHIPMODE(flatbuffers::Offset<flatbuffers::String> L_SHIPMODE) {
    fbb_.AddOffset(Row::VT_L_SHIPMODE, L_SHIPMODE);
  }
  void add_L_COMMENT(flatbuffers::Offset<flatbuffers::String> L_COMMENT) {
    fbb_.AddOffset(Row::VT_L_COMMENT, L_COMMENT);
  }
  explicit RowBuilder(flatbuffers::FlatBufferBuilder &_fbb)
        : fbb_(_fbb) {
    start_ = fbb_.StartTable();
  }
  RowBuilder &operator=(const RowBuilder &);
  flatbuffers::Offset<Row> Finish() {
    const auto end = fbb_.EndTable(start_);
    auto o = flatbuffers::Offset<Row>(end);
    return o;
  }
};

inline flatbuffers::Offset<Row> CreateRow(
    flatbuffers::FlatBufferBuilder &_fbb,
    int32_t L_ORDERKEY = 0,
    int32_t L_PARTKEY = 0,
    int32_t L_SUPPKEY = 0,
    int32_t L_LINENUMBER = 0,
    float L_QUANTITY = 0.0f,
    float L_EXTENDEDPRICE = 0.0f,
    float L_DISCOUNT = 0.0f,
    float L_TAX = 0.0f,
    int32_t L_RETURNFLAG = 0,
    int32_t L_LINESTATUS = 0,
    const Date *L_SHIPDATE = 0,
    const Date *L_RECEIPTDATE = 0,
    const Date *L_COMMITDATE = 0,
    flatbuffers::Offset<flatbuffers::String> L_SHIPINSTRUCT = 0,
    flatbuffers::Offset<flatbuffers::String> L_SHIPMODE = 0,
    flatbuffers::Offset<flatbuffers::String> L_COMMENT = 0) {
  RowBuilder builder_(_fbb);
  builder_.add_L_COMMENT(L_COMMENT);
  builder_.add_L_SHIPMODE(L_SHIPMODE);
  builder_.add_L_SHIPINSTRUCT(L_SHIPINSTRUCT);
  builder_.add_L_COMMITDATE(L_COMMITDATE);
  builder_.add_L_RECEIPTDATE(L_RECEIPTDATE);
  builder_.add_L_SHIPDATE(L_SHIPDATE);
  builder_.add_L_LINESTATUS(L_LINESTATUS);
  builder_.add_L_RETURNFLAG(L_RETURNFLAG);
  builder_.add_L_TAX(L_TAX);
  builder_.add_L_DISCOUNT(L_DISCOUNT);
  builder_.add_L_EXTENDEDPRICE(L_EXTENDEDPRICE);
  builder_.add_L_QUANTITY(L_QUANTITY);
  builder_.add_L_LINENUMBER(L_LINENUMBER);
  builder_.add_L_SUPPKEY(L_SUPPKEY);
  builder_.add_L_PARTKEY(L_PARTKEY);
  builder_.add_L_ORDERKEY(L_ORDERKEY);
  return builder_.Finish();
}

inline flatbuffers::Offset<Row> CreateRowDirect(
    flatbuffers::FlatBufferBuilder &_fbb,
    int32_t L_ORDERKEY = 0,
    int32_t L_PARTKEY = 0,
    int32_t L_SUPPKEY = 0,
    int32_t L_LINENUMBER = 0,
    float L_QUANTITY = 0.0f,
    float L_EXTENDEDPRICE = 0.0f,
    float L_DISCOUNT = 0.0f,
    float L_TAX = 0.0f,
    int32_t L_RETURNFLAG = 0,
    int32_t L_LINESTATUS = 0,
    const Date *L_SHIPDATE = 0,
    const Date *L_RECEIPTDATE = 0,
    const Date *L_COMMITDATE = 0,
    const char *L_SHIPINSTRUCT = nullptr,
    const char *L_SHIPMODE = nullptr,
    const char *L_COMMENT = nullptr) {
  return Tables::CreateRow(
      _fbb,
      L_ORDERKEY,
      L_PARTKEY,
      L_SUPPKEY,
      L_LINENUMBER,
      L_QUANTITY,
      L_EXTENDEDPRICE,
      L_DISCOUNT,
      L_TAX,
      L_RETURNFLAG,
      L_LINESTATUS,
      L_SHIPDATE,
      L_RECEIPTDATE,
      L_COMMITDATE,
      L_SHIPINSTRUCT ? _fbb.CreateString(L_SHIPINSTRUCT) : 0,
      L_SHIPMODE ? _fbb.CreateString(L_SHIPMODE) : 0,
      L_COMMENT ? _fbb.CreateString(L_COMMENT) : 0);
}

inline const Tables::Table *GetTable(const void *buf) {
  return flatbuffers::GetRoot<Tables::Table>(buf);
}

inline const Tables::Table *GetSizePrefixedTable(const void *buf) {
  return flatbuffers::GetSizePrefixedRoot<Tables::Table>(buf);
}

inline bool VerifyTableBuffer(
    flatbuffers::Verifier &verifier) {
  return verifier.VerifyBuffer<Tables::Table>(nullptr);
}

inline bool VerifySizePrefixedTableBuffer(
    flatbuffers::Verifier &verifier) {
  return verifier.VerifySizePrefixedBuffer<Tables::Table>(nullptr);
}

inline void FinishTableBuffer(
    flatbuffers::FlatBufferBuilder &fbb,
    flatbuffers::Offset<Tables::Table> root) {
  fbb.Finish(root);
}

inline void FinishSizePrefixedTableBuffer(
    flatbuffers::FlatBufferBuilder &fbb,
    flatbuffers::Offset<Tables::Table> root) {
  fbb.FinishSizePrefixed(root);
}

}  // namespace Tables

#endif  // FLATBUFFERS_GENERATED_MULTIROWFLATBUFFER_TABLES_H_