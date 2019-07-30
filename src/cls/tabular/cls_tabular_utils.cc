/*
* Copyright (C) 2018 The Regents of the University of California
* All Rights Reserved
*
* This library can redistribute it and/or modify under the terms
* of the GNU Lesser General Public License Version 2.1 as published
* by the Free Software Foundation.
*
*/


#include "cls_tabular_utils.h"


namespace Tables {

int processArrow(
    std::shared_ptr<arrow::Table>* table,
    schema_vec& tbl_schema,
    schema_vec& query_schema,
    predicate_vec& preds,
    const char* dataptr,
    const size_t datasz,
    std::string& errmsg,
    const std::vector<uint32_t>& row_nums)
{
    std::shared_ptr<arrow::Buffer> buffer;
    std::shared_ptr<arrow::Table> proj_table, temp_table;
    std::string str_data(dataptr, datasz);
    arrow::Buffer::FromString(str_data, &buffer);
    extract_arrow_from_buffer(&proj_table, buffer);

    auto schema = proj_table->schema();
    auto metadata = schema->metadata();

    // identify the max col idx, to prevent flexbuf vector oob error
    int col_idx_max = -1;
    for (auto it = tbl_schema.begin(); it != tbl_schema.end(); ++it) {
        if (it->idx > col_idx_max)
            col_idx_max = it->idx;
    }

    bool project_all = std::equal (tbl_schema.begin(), tbl_schema.end(),
                                   query_schema.begin(), compareColInfo);

    //    proj_table = std::make_shared<arrow::Table>(proj_table);
    if (!project_all) {
        for (auto it = query_schema.begin(); it != query_schema.end(); ++it) {
            for (auto it2 = tbl_schema.begin(); it2 != tbl_schema.end(); ++it2) {
                if (!it2->compareName((*it).name))
                    proj_table->RemoveColumn((*it2).idx, &temp_table);
            }
        }
    }
    proj_table = temp_table;
    std::shared_ptr<arrow::KeyValueMetadata> proj_metadata (new arrow::KeyValueMetadata);
    // Add skyhook metadata to arrow metadata.
    proj_metadata->Append(ToString(METADATA_SKYHOOK_VERSION),
                          metadata->value(METADATA_SKYHOOK_VERSION));
    proj_metadata->Append(ToString(METADATA_DATA_SCHEMA_VERSION),
                          metadata->value(METADATA_DATA_SCHEMA_VERSION));
    proj_metadata->Append(ToString(METADATA_DATA_STRUCTURE_VERSION),
                          metadata->value(METADATA_DATA_STRUCTURE_VERSION));
    proj_metadata->Append(ToString(METADATA_DATA_FORMAT_TYPE),
                          metadata->value(METADATA_DATA_FORMAT_TYPE));
    proj_metadata->Append(ToString(METADATA_DATA_SCHEMA),
                          schemaToString(query_schema));
    proj_metadata->Append(ToString(METADATA_DB_SCHEMA),
                          metadata->value(METADATA_DB_SCHEMA));
    proj_metadata->Append(ToString(METADATA_TABLE_NAME),
                          metadata->value(METADATA_TABLE_NAME));
    proj_metadata->Append(ToString(METADATA_NUM_ROWS),
                          metadata->value(METADATA_NUM_ROWS));

    *table = proj_table->ReplaceSchemaMetadata(proj_metadata);
    return 0;
}

int processSkyFb(
    flatbuffers::FlatBufferBuilder& flatbldr,
    schema_vec& data_schema,
    schema_vec& query_schema,
    predicate_vec& preds,
    const char* fb,
    const size_t fb_size,
    std::string& errmsg,
    const std::vector<uint32_t>& row_nums)
{
    int errcode = 0;
    delete_vector dead_rows;
    std::vector<flatbuffers::Offset<Tables::Record>> offs;
    sky_root root = getSkyRoot(fb, fb_size, SFT_FLATBUF_FLEX_ROW);

    // identify the max col idx, to prevent flexbuf vector oob error
    int col_idx_max = -1;
    for (auto it=data_schema.begin(); it!=data_schema.end(); ++it) {
        if (it->idx > col_idx_max)
            col_idx_max = it->idx;
    }

    bool project_all = std::equal(data_schema.begin(), data_schema.end(),
                                  query_schema.begin(), compareColInfo);

    // build the flexbuf with computed aggregates, aggs are computed for
    // each row that passes, and added to flexbuf after loop below.
    bool encode_aggs = false;
    if (hasAggPreds(preds)) encode_aggs = true;
    bool encode_rows = !encode_aggs;

    // determines if we process specific rows or all rows, since
    // row_nums vector is optional parameter - default process all rows.
    bool process_all_rows = true;
    uint32_t nrows = root.nrows;
    if (!row_nums.empty()) {
        process_all_rows = false;  // process specified row numbers only
        nrows = row_nums.size();
    }

    // 1. check the preds for passing
    // 2a. accumulate agg preds (return flexbuf built after all rows) or
    // 2b. build the return flatbuf inline below from each row's projection
    for (uint32_t i = 0; i < nrows; i++) {

        // process row i or the specified row number
        uint32_t rnum = 0;
        if (process_all_rows) rnum = i;
        else rnum = row_nums[i];
        if (rnum > root.nrows) {
            errmsg += "ERROR: rnum(" + std::to_string(rnum) +
                      ") > root.nrows(" + to_string(root.nrows) + ")";
            return RowIndexOOB;
        }

         // skip dead rows.
        if (root.delete_vec[rnum] == 1) continue;

        // get a skyhook record struct
        sky_rec rec = getSkyRec(static_cast<row_offs>(root.data_vec)->Get(rnum));

        // apply predicates to this record
        if (!preds.empty()) {
            bool pass = applyPredicates(preds, rec);
            if (!pass) continue;  // skip non matching rows.
        }

        // note: agg preds are accumlated in the predicate itself during
        // applyPredicates above, then later added to result fb outside
        // of this loop (i.e., they are not encoded into the result fb yet)
        // thus we can skip the below encoding of rows into the result fb
        // and just continue accumulating agg preds in this processing loop.
        if (!encode_rows) continue;

        if (project_all) {
            // TODO:  just pass through row table offset to new data_vec
            // (which is also type offs), do not rebuild row table and flexbuf
        }

        // build the return projection for this row.
        auto row = rec.data.AsVector();
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flatbuffers::Offset<flatbuffers::Vector<unsigned char>> datavec;

        flexbldr->Vector([&]() {

            // iter over the query schema, locating it within the data schema
            for (auto it=query_schema.begin();
                      it!=query_schema.end() && !errcode; ++it) {
                col_info col = *it;
                if (col.idx < AGG_COL_LAST or col.idx > col_idx_max) {
                    errcode = TablesErrCodes::RequestedColIndexOOB;
                    errmsg.append("ERROR processSkyFb(): table=" +
                            root.table_name + "; rid=" +
                            std::to_string(rec.RID) + " col.idx=" +
                            std::to_string(col.idx) + " OOB.");

                } else {

                    switch(col.type) {  // encode data val into flexbuf

                        case SDT_INT8:
                            flexbldr->Add(row[col.idx].AsInt8());
                            break;
                        case SDT_INT16:
                            flexbldr->Add(row[col.idx].AsInt16());
                            break;
                        case SDT_INT32:
                            flexbldr->Add(row[col.idx].AsInt32());
                            break;
                        case SDT_INT64:
                            flexbldr->Add(row[col.idx].AsInt64());
                            break;
                        case SDT_UINT8:
                            flexbldr->Add(row[col.idx].AsUInt8());
                            break;
                        case SDT_UINT16:
                            flexbldr->Add(row[col.idx].AsUInt16());
                            break;
                        case SDT_UINT32:
                            flexbldr->Add(row[col.idx].AsUInt32());
                            break;
                        case SDT_UINT64:
                            flexbldr->Add(row[col.idx].AsUInt64());
                            break;
                        case SDT_CHAR:
                            flexbldr->Add(row[col.idx].AsInt8());
                            break;
                        case SDT_UCHAR:
                            flexbldr->Add(row[col.idx].AsUInt8());
                            break;
                        case SDT_BOOL:
                            flexbldr->Add(row[col.idx].AsBool());
                            break;
                        case SDT_FLOAT:
                            flexbldr->Add(row[col.idx].AsFloat());
                            break;
                        case SDT_DOUBLE:
                            flexbldr->Add(row[col.idx].AsDouble());
                            break;
                        case SDT_DATE:
                            flexbldr->Add(row[col.idx].AsString().str());
                            break;
                        case SDT_STRING:
                            flexbldr->Add(row[col.idx].AsString().str());
                            break;
                        default: {
                            errcode = TablesErrCodes::UnsupportedSkyDataType;
                            errmsg.append("ERROR processSkyFb(): table=" +
                                    root.table_name + "; rid=" +
                                    std::to_string(rec.RID) + " col.type=" +
                                    std::to_string(col.type) +
                                    " UnsupportedSkyDataType.");
                        }
                    }
                }
            }
        });

        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // TODO: update nullbits
        auto nullbits = flatbldr.CreateVector(rec.nullbits);
        flatbuffers::Offset<Tables::Record> row_off = \
                Tables::CreateRecord(flatbldr, rec.RID, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
    }

    // here we build the return flatbuf result with agg values that were
    // accumulated above in applyPredicates (agg predicates do not return
    // true false but update their internal values each time processed
    if (encode_aggs) { //  encode accumulated agg pred val into return flexbuf
        PredicateBase* pb;
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flexbldr->Vector([&]() {
            for (auto itp = preds.begin(); itp != preds.end(); ++itp) {

                // assumes preds appear in same order as return schema
                if (!(*itp)->isGlobalAgg()) continue;
                pb = *itp;
                switch(pb->colType()) {  // encode agg data val into flexbuf
                    case SDT_INT64: {
                        TypedPredicate<int64_t>* p = \
                                dynamic_cast<TypedPredicate<int64_t>*>(pb);
                        int64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_UINT64: {
                        TypedPredicate<uint64_t>* p = \
                                dynamic_cast<TypedPredicate<uint64_t>*>(pb);
                        uint64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_FLOAT: {
                        TypedPredicate<float>* p = \
                                dynamic_cast<TypedPredicate<float>*>(pb);
                        float agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_DOUBLE: {
                        TypedPredicate<double>* p = \
                                dynamic_cast<TypedPredicate<double>*>(pb);
                        double agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    default:  assert(UnsupportedAggDataType==0);
                }
            }
        });
        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // assume no nullbits in the agg results. ?
        nullbits_vector nb(2,0);
        auto nullbits = flatbldr.CreateVector(nb);
        int RID = -1;  // agg recs only, since these are derived data
        flatbuffers::Offset<Tables::Record> row_off = \
            Tables::CreateRecord(flatbldr, RID, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
    }

    // now build the return ROOT flatbuf wrapper
    std::string query_schema_str;
    for (auto it = query_schema.begin(); it != query_schema.end(); ++it) {
        query_schema_str.append(it->toString() + "\n");
    }

    auto return_data_schema = flatbldr.CreateString(query_schema_str);
    auto db_schema_name = flatbldr.CreateString(root.db_schema_name);
    auto table_name = flatbldr.CreateString(root.table_name);
    auto delete_v = flatbldr.CreateVector(dead_rows);
    auto rows_v = flatbldr.CreateVector(offs);

    auto table = CreateTable(
        flatbldr,
        root.data_format_type,
        root.skyhook_version,
        root.data_structure_version,
        root.data_schema_version,
        return_data_schema,
        db_schema_name,
        table_name,
        delete_v,
        rows_v,
        offs.size());

    // NOTE: the fb may be incomplete/empty, but must finish() else internal
    // fb lib assert finished() fails, hence we must always return a valid fb
    // and catch any ret error code upstream
    flatbldr.Finish(table);

    return errcode;
}

int processSkyFb_fbu_cols(
    flatbuffers::FlatBufferBuilder& flatbldr,
    schema_vec& data_schema,
    schema_vec& query_schema,
    predicate_vec& preds,
    const char* fb,
    const size_t fb_size,
    std::string& errmsg,
    const std::vector<uint32_t>& row_nums)
{
    int errcode = 0;
    delete_vector dead_rows;
    std::vector<flatbuffers::Offset<Tables::Record>> offs;
    sky_root root = getSkyRoot(fb, fb_size, SFT_FLATBUF_UNION_COL);

    // identify the max col idx, to prevent flexbuf vector oob error
    int col_idx_max = -1;
    for (auto it=data_schema.begin(); it!=data_schema.end(); ++it) {
        if (it->idx > col_idx_max)
            col_idx_max = it->idx;
    }

    bool project_all = std::equal(data_schema.begin(), data_schema.end(),
                                  query_schema.begin(), compareColInfo);

    // build the flexbuf with computed aggregates, aggs are computed for
    // each row that passes, and added to flexbuf after loop below.
    bool encode_aggs = false;
    if (hasAggPreds(preds)) encode_aggs = true;
    bool encode_rows = !encode_aggs;

    // determines if we process specific rows or all rows, since
    // row_nums vector is optional parameter - default process all rows.
    bool process_all_rows = true;
    uint32_t nrows = root.nrows;
    if (!row_nums.empty()) {
        process_all_rows = false;  // process specified row numbers only
        nrows = row_nums.size();
    }

    // 1. check the preds for passing
    // 2a. accumulate agg preds (return flexbuf built after all rows) or
    // 2b. build the return flatbuf inline below from each row's projection
    for (uint32_t i = 0; i < nrows; i++) {

        // process row i or the specified row number
        uint32_t rnum = 0;
        if (process_all_rows) rnum = i;
        else rnum = row_nums[i];
        if (rnum > root.nrows) {
            errmsg += "ERROR: rnum(" + std::to_string(rnum) +
                      ") > root.nrows(" + to_string(root.nrows) + ")";
            return RowIndexOOB;
        }

        // skip dead rows.
        if (root.delete_vec[rnum] == 1) continue;

        // note: agg preds are accumlated in the predicate itself during
        // applyPredicates above, then later added to result fb outside
        // of this loop (i.e., they are not encoded into the result fb yet)
        // thus we can skip the below encoding of rows into the result fb
        // and just continue accumulating agg preds in this processing loop.
        // note: skipping doesn't work with this implementation of col processing.
        //if (!encode_rows) continue;

        if (project_all) {
            // TODO:  just pass through row table offset to new data_vec
            // (which is also type offs), do not rebuild row table and flexbuf
        }

        // build the return projection for this row.
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flatbuffers::Offset<flatbuffers::Vector<unsigned char>> datavec;

        int this_rid = 0 ;
        flexbldr->Vector([&]() {
            // iter over the query schema, locating it within the data schema
            for (auto it=query_schema.begin();
                      it!=query_schema.end() && !errcode; ++it) {

                // get column metadata
                col_info colmeta = *it;

                // get a skyhook col struct
                sky_col_fbu skycol = getSkyCol_fbu( root, colmeta.idx );
                auto this_col       = skycol.data_fbu_col ;
                //auto this_col_name  = this_col->col_name() ;
                //auto this_col_index = this_col->col_index() ;
                auto curr_col_data  = this_col->data() ;
                auto curr_col_data_rids = this_col->RIDs() ;
                this_rid = curr_col_data_rids->Get( rnum ) ;

                if (colmeta.idx < AGG_COL_LAST or colmeta.idx > col_idx_max) {
                    errcode = TablesErrCodes::RequestedColIndexOOB;
                    errmsg.append("ERROR processSkyFb(): table=" +
                            root.table_name + "; rid=" +
                            std::to_string(this_rid) + " colmeta.idx=" +
                            std::to_string(colmeta.idx) + " OOB.");
                } else {
                    switch(colmeta.type) {  // encode data val into flexbuf
                        //case SDT_INT8:
                        //    flexbldr->Add(row[col.idx].AsInt8());
                        //    break;
                        //case SDT_INT16:
                        //    flexbldr->Add(row[col.idx].AsInt16());
                        //    break;
                        //case SDT_INT32:
                        //    flexbldr->Add(row[col.idx].AsInt32());
                        //    break;
                        //case SDT_INT64:
                        //    flexbldr->Add(row[col.idx].AsInt64());
                        //    break;
                        //case SDT_UINT8:
                        //    flexbldr->Add(row[col.idx].AsUInt8());
                        //    break;
                        //case SDT_UINT16:
                        //    flexbldr->Add(row[col.idx].AsUInt16());
                        //    break;
                        //case SDT_UINT32:
                        //    flexbldr->Add(row[col.idx].AsUInt32());
                        //    break;
                        case SDT_UINT64: {
                            auto column_of_data = 
                                static_cast< const Tables::SDT_UINT64_FBU* >( curr_col_data ) ;
                            auto data_at_row = column_of_data->data()->Get( rnum ) ;
                            std::cout << "0=" << std::to_string( data_at_row ) << std::endl ;
                            flexbldr->Add( data_at_row );
                            break;
                        }
                        //case SDT_CHAR:
                        //    flexbldr->Add(row[col.idx].AsInt8());
                        //    break;
                        //case SDT_UCHAR:
                        //    flexbldr->Add(row[col.idx].AsUInt8());
                        //    break;
                        //case SDT_BOOL:
                        //    flexbldr->Add(row[col.idx].AsBool());
                        //    break;
                        case SDT_FLOAT: {
                            auto column_of_data = 
                                static_cast< const Tables::SDT_FLOAT_FBU* >( curr_col_data ) ;
                            auto data_at_row = column_of_data->data()->Get( rnum ) ;
                            std::cout << " 0= " << std::to_string( data_at_row ) << std::endl ;
                            flexbldr->Add( data_at_row );
                            break;
                        }
                        //case SDT_DOUBLE:
                        //    flexbldr->Add(row[col.idx].AsDouble());
                        //    break;
                        //case SDT_DATE:
                        //    flexbldr->Add(row[col.idx].AsString().str());
                        //    break;
                        case SDT_STRING: {
                            auto column_of_data = 
                                static_cast< const Tables::SDT_STRING_FBU* >( curr_col_data ) ;
                            auto data_at_row = column_of_data->data()->Get( rnum )->str() ;
                            std::cout << " 0= " << data_at_row << std::endl ;
                            flexbldr->Add( data_at_row );
                            break;
                        }
                        default: {
                            errcode = TablesErrCodes::UnsupportedSkyDataType;
                            errmsg.append("ERROR processSkyFb(): table=" +
                                    root.table_name + "; rid=" +
                                    std::to_string(rnum) + " colmeta.type=" +
                                    std::to_string(colmeta.type) +
                                    " UnsupportedSkyDataType.");
                        } //default
                    } //switch
                } //ifelse
            } //for query column
        }); //flex builder Vector

        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // get the sky_rec version of the extracted row
        // TODO: there's gotta be a better way.
        flatbuffers::FlatBufferBuilder tmp_builder(1024) ;
        std::vector< uint64_t > nv ( 2, 0 ) ;
        auto nv_fb          = tmp_builder.CreateVector( nv ) ;
        auto extracted_data = tmp_builder.CreateVector(flexbldr->GetBuffer());
        flatbuffers::Offset<Tables::Record> extracted_fb = 
                Tables::CreateRecord(tmp_builder, this_rid, nv_fb, extracted_data);
        std::vector< flatbuffers::Offset<Tables::Record> > rows ;
        rows.push_back( extracted_fb ) ;
        auto rows_fb = tmp_builder.CreateVector( rows ) ;
        auto t = Tables::CreateTable( tmp_builder, 0, 0, 0, 0, 0, 0, 0, 0, rows_fb, 0);
        tmp_builder.Finish( t ) ;
        auto buffptr = tmp_builder.GetBufferPointer() ;
        auto root = Tables::GetTable( buffptr );
        auto data_rec = root->rows()->Get(0) ; //there is only one
        sky_rec skyrec( this_rid, nv, data_rec->data_flexbuffer_root() ) ;

        // apply predicates to this record
        if (!preds.empty()) {
            bool pass = applyPredicates(preds, skyrec);
            if (!pass) continue;  // skip non matching rows.
        }

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // TODO: replace this with actual nullbit tallies
        std::vector< uint64_t > nullbits_vector ( 2, 0 ) ;
        auto nullbits = flatbldr.CreateVector(nullbits_vector);
        flatbuffers::Offset<Tables::Record> row_off = 
                Tables::CreateRecord(flatbldr, rnum, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
    } //for row number

    // here we build the return flatbuf result with agg values that were
    // accumulated above in applyPredicates (agg predicates do not return
    // true false but update their internal values each time processed
    if (encode_aggs) { //  encode accumulated agg pred val into return flexbuf
        PredicateBase* pb;
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flexbldr->Vector([&]() {
            for (auto itp = preds.begin(); itp != preds.end(); ++itp) {
                // assumes preds appear in same order as return schema
                if (!(*itp)->isGlobalAgg()) continue;
                pb = *itp;
                switch(pb->colType()) {  // encode agg data val into flexbuf
                    case SDT_INT64: {
                        TypedPredicate<int64_t>* p = \
                                dynamic_cast<TypedPredicate<int64_t>*>(pb);
                        int64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_UINT64: {
                        TypedPredicate<uint64_t>* p = \
                                dynamic_cast<TypedPredicate<uint64_t>*>(pb);
                        uint64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_FLOAT: {
                        TypedPredicate<float>* p = \
                                dynamic_cast<TypedPredicate<float>*>(pb);
                        float agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_DOUBLE: {
                        TypedPredicate<double>* p = \
                                dynamic_cast<TypedPredicate<double>*>(pb);
                        double agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    default:  assert(UnsupportedAggDataType==0);
                }
            }
        });
        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // assume no nullbits in the agg results. ?
        nullbits_vector nb(2,0);
        auto nullbits = flatbldr.CreateVector(nb);
        int RID = -1;  // agg recs only, since these are derived data
        flatbuffers::Offset<Tables::Record> row_off = \
            Tables::CreateRecord(flatbldr, RID, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
        //std::cout << "aggggggggggggggggggssssssssssssssssss" << std::endl ;
        //exit(1) ;
/* testing...
...testing */
    } //if aggs

    // now build the return ROOT flatbuf wrapper
    std::string query_schema_str;
    for (auto it = query_schema.begin(); it != query_schema.end(); ++it) {
        query_schema_str.append(it->toString() + "\n");
    }

    auto return_data_schema = flatbldr.CreateString(query_schema_str);
    auto db_schema_name     = flatbldr.CreateString(root.db_schema_name);
    auto table_name         = flatbldr.CreateString(root.table_name);
    auto delete_v           = flatbldr.CreateVector(dead_rows);
    auto rows_v             = flatbldr.CreateVector(offs);

    auto table = CreateTable(
        flatbldr,
        root.data_format_type,
        root.skyhook_version,
        root.data_structure_version,
        root.data_schema_version,
        return_data_schema,
        db_schema_name,
        table_name,
        delete_v,
        rows_v,
        offs.size());

    // NOTE: the fb may be incomplete/empty, but must finish() else internal
    // fb lib assert finished() fails, hence we must always return a valid fb
    // and catch any ret error code upstream
    flatbldr.Finish(table);
    std::cout << "processSkyFb_fbu_cols done." << std::endl ;

    return errcode;
} //processSkyFb_fbu_cols

int processSkyFb_fbu_rows(
    flatbuffers::FlatBufferBuilder& flatbldr,
    schema_vec& data_schema,
    schema_vec& query_schema,
    predicate_vec& preds,
    const char* fb,
    const size_t fb_size,
    std::string& errmsg,
    const std::vector<uint32_t>& row_nums)
{
    int errcode = 0;
    delete_vector dead_rows;
    std::vector<flatbuffers::Offset<Tables::Record>> offs;
    sky_root root = getSkyRoot(fb, fb_size, SFT_FLATBUF_UNION_ROW);

    // identify the max col idx, to prevent flexbuf vector oob error
    int col_idx_max = -1;
    for (auto it=data_schema.begin(); it!=data_schema.end(); ++it) {
        if (it->idx > col_idx_max)
            col_idx_max = it->idx;
    }

    bool project_all = std::equal(data_schema.begin(), data_schema.end(),
                                  query_schema.begin(), compareColInfo);

    // build the flexbuf with computed aggregates, aggs are computed for
    // each row that passes, and added to flexbuf after loop below.
    bool encode_aggs = false;
    if (hasAggPreds(preds)) encode_aggs = true;
    bool encode_rows = !encode_aggs;

    // determines if we process specific rows or all rows, since
    // row_nums vector is optional parameter - default process all rows.
    bool process_all_rows = true;
    uint32_t nrows = root.nrows;
    if (!row_nums.empty()) {
        process_all_rows = false;  // process specified row numbers only
        nrows = row_nums.size();
    }

    // 1. check the preds for passing
    // 2a. accumulate agg preds (return flexbuf built after all rows) or
    // 2b. build the return flatbuf inline below from each row's projection
    for (uint32_t i = 0; i < nrows; i++) {

        // process row i or the specified row number
        uint32_t rnum = 0;
        if (process_all_rows) rnum = i;
        else rnum = row_nums[i];
        if (rnum > root.nrows) {
            errmsg += "ERROR: rnum(" + std::to_string(rnum) +
                      ") > root.nrows(" + to_string(root.nrows) + ")";
            return RowIndexOOB;
        }

        // skip dead rows.
        if (root.delete_vec[rnum] == 1) continue;

        // get a skyhook record struct
        sky_rec_fbu rec = getSkyRec_fbu( root, rnum );

        // apply predicates to this record
        if (!preds.empty()) {
            bool pass = applyPredicates_fbu_row(preds, rec);
            if (!pass) continue;  // skip non matching rows.
        }

        // note: agg preds are accumlated in the predicate itself during
        // applyPredicates above, then later added to result fb outside
        // of this loop (i.e., they are not encoded into the result fb yet)
        // thus we can skip the below encoding of rows into the result fb
        // and just continue accumulating agg preds in this processing loop.
        if (!encode_rows) continue;

        if (project_all) {
            // TODO:  just pass through row table offset to new data_vec
            // (which is also type offs), do not rebuild row table and flexbuf
        }

        // build the return projection for this row.
        auto row = rec.data_fbu_rows ;
        //auto row = rec.data.AsVector();
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flatbuffers::Offset<flatbuffers::Vector<unsigned char>> datavec;

        flexbldr->Vector([&]() {
            // iter over the query schema, locating it within the data schema
            for (auto it=query_schema.begin();
                      it!=query_schema.end() && !errcode; ++it) {
                col_info col = *it;
                if (col.idx < AGG_COL_LAST or col.idx > col_idx_max) {
                    errcode = TablesErrCodes::RequestedColIndexOOB;
                    errmsg.append("ERROR processSkyFb(): table=" +
                            root.table_name + "; rid=" +
                            std::to_string(rec.RID) + " col.idx=" +
                            std::to_string(col.idx) + " OOB.");
                } else {
                    switch(col.type) {  // encode data val into flexbuf
                        //case SDT_INT8:
                        //    flexbldr->Add(row[col.idx].AsInt8());
                        //    break;
                        //case SDT_INT16:
                        //    flexbldr->Add(row[col.idx].AsInt16());
                        //    break;
                        //case SDT_INT32:
                        //    flexbldr->Add(row[col.idx].AsInt32());
                        //    break;
                        //case SDT_INT64:
                        //    flexbldr->Add(row[col.idx].AsInt64());
                        //    break;
                        //case SDT_UINT8:
                        //    flexbldr->Add(row[col.idx].AsUInt8());
                        //    break;
                        //case SDT_UINT16:
                        //    flexbldr->Add(row[col.idx].AsUInt16());
                        //    break;
                        //case SDT_UINT32:
                        //    flexbldr->Add(row[col.idx].AsUInt32());
                        //    break;
                        case SDT_UINT64: {
                            auto int_col_data =
                              static_cast< const Tables::SDT_UINT64_FBU* >( row->Get(col.idx) ) ;
                            auto data = int_col_data->data()->Get(0) ;
                            flexbldr->Add(data);
                            break;
                        }
                        //case SDT_CHAR:
                        //    flexbldr->Add(row[col.idx].AsInt8());
                        //    break;
                        //case SDT_UCHAR:
                        //    flexbldr->Add(row[col.idx].AsUInt8());
                        //    break;
                        //case SDT_BOOL:
                        //    flexbldr->Add(row[col.idx].AsBool());
                        //    break;
                        case SDT_FLOAT: {
                            auto float_col_data =
                              static_cast< const Tables::SDT_FLOAT_FBU* >( row->Get(col.idx) ) ;
                            auto data = float_col_data->data()->Get(0) ;
                            flexbldr->Add(data);
                            break;
                        }
                        //case SDT_DOUBLE:
                        //    flexbldr->Add(row[col.idx].AsDouble());
                        //    break;
                        //case SDT_DATE:
                        //    flexbldr->Add(row[col.idx].AsString().str());
                        //    break;
                        case SDT_STRING: {
                            auto string_col_data =
                              static_cast< const Tables::SDT_STRING_FBU* >( row->Get(col.idx) ) ;
                            auto data = string_col_data->data()->Get(0)->str() ;
                            flexbldr->Add(data);
                            break;
                        }
                        default: {
                            errcode = TablesErrCodes::UnsupportedSkyDataType;
                            errmsg.append("ERROR processSkyFb(): table=" +
                                    root.table_name + "; rid=" +
                                    std::to_string(rec.RID) + " col.type=" +
                                    std::to_string(col.type) +
                                    " UnsupportedSkyDataType.");
                        } //default
                    } //switch
                } //ifelse
            } //for
        }); //flex builder Vector

        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // TODO: update nullbits
        auto nullbits = flatbldr.CreateVector(rec.nullbits);
        flatbuffers::Offset<Tables::Record> row_off = \
                Tables::CreateRecord(flatbldr, rec.RID, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
    } //for

    // here we build the return flatbuf result with agg values that were
    // accumulated above in applyPredicates (agg predicates do not return
    // true false but update their internal values each time processed
    if (encode_aggs) { //  encode accumulated agg pred val into return flexbuf
        PredicateBase* pb;
        flexbuffers::Builder *flexbldr = new flexbuffers::Builder();
        flexbldr->Vector([&]() {
            for (auto itp = preds.begin(); itp != preds.end(); ++itp) {
                // assumes preds appear in same order as return schema
                if (!(*itp)->isGlobalAgg()) continue;
                pb = *itp;
                switch(pb->colType()) {  // encode agg data val into flexbuf
                    case SDT_INT64: {
                        TypedPredicate<int64_t>* p = \
                                dynamic_cast<TypedPredicate<int64_t>*>(pb);
                        int64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_UINT64: {
                        TypedPredicate<uint64_t>* p = \
                                dynamic_cast<TypedPredicate<uint64_t>*>(pb);
                        uint64_t agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_FLOAT: {
                        TypedPredicate<float>* p = \
                                dynamic_cast<TypedPredicate<float>*>(pb);
                        float agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    case SDT_DOUBLE: {
                        TypedPredicate<double>* p = \
                                dynamic_cast<TypedPredicate<double>*>(pb);
                        double agg_val = p->Val();
                        flexbldr->Add(agg_val);
                        break;
                    }
                    default:  assert(UnsupportedAggDataType==0);
                }
            }
        });
        // finalize the row's projected data within our flexbuf
        flexbldr->Finish();

        // build the return ROW flatbuf that contains the flexbuf data
        auto row_data = flatbldr.CreateVector(flexbldr->GetBuffer());
        delete flexbldr;

        // assume no nullbits in the agg results. ?
        nullbits_vector nb(2,0);
        auto nullbits = flatbldr.CreateVector(nb);
        int RID = -1;  // agg recs only, since these are derived data
        flatbuffers::Offset<Tables::Record> row_off = \
            Tables::CreateRecord(flatbldr, RID, nullbits, row_data);

        // Continue building the ROOT flatbuf's dead vector and rowOffsets vec
        dead_rows.push_back(0);
        offs.push_back(row_off);
    } //if aggs

    // now build the return ROOT flatbuf wrapper
    std::string query_schema_str;
    for (auto it = query_schema.begin(); it != query_schema.end(); ++it) {
        query_schema_str.append(it->toString() + "\n");
    }

    auto return_data_schema = flatbldr.CreateString(query_schema_str);
    auto db_schema_name = flatbldr.CreateString(root.db_schema_name);
    auto table_name = flatbldr.CreateString(root.table_name);
    auto delete_v = flatbldr.CreateVector(dead_rows);
    auto rows_v = flatbldr.CreateVector(offs);

    auto table = CreateTable(
        flatbldr,
        root.data_format_type,
        root.skyhook_version,
        root.data_structure_version,
        root.data_schema_version,
        return_data_schema,
        db_schema_name,
        table_name,
        delete_v,
        rows_v,
        offs.size());

    // NOTE: the fb may be incomplete/empty, but must finish() else internal
    // fb lib assert finished() fails, hence we must always return a valid fb
    // and catch any ret error code upstream
    flatbldr.Finish(table);

    return errcode;
} //processSkyFb_fbu_rows

// simple converstion from schema to its str representation.
std::string schemaToString(schema_vec schema) {
    std::string s;
    for (auto it = schema.begin(); it != schema.end(); ++it)
        s.append(it->toString() + "\n");
    return s;
}

schema_vec schemaFromColNames(schema_vec &current_schema,
                              std::string col_names) {
    schema_vec schema;
    boost::trim(col_names);
    if (col_names == PROJECT_DEFAULT) {
        for (auto it=current_schema.begin(); it!=current_schema.end(); ++it) {
            schema.push_back(*it);
        }
    }
    else if (col_names == RID_INDEX) {
        col_info ci(RID_COL_INDEX, SDT_UINT64, true, false, RID_INDEX);
        schema.push_back(ci);

    }
    else {
        vector<std::string> cols;
        boost::split(cols, col_names, boost::is_any_of(","),
                     boost::token_compress_on);

        // build return schema elems in order of colnames provided.
        for (auto it=cols.begin(); it!=cols.end(); ++it) {
            for (auto it2=current_schema.begin();
                      it2!=current_schema.end(); ++it2) {
                if (it2->compareName(*it))
                    schema.push_back(*it2);
            }
        }
    }
    return schema;
}

// schema string expects the format in cls_tabular_utils.h
// see lineitem_test_schema
schema_vec schemaFromString(std::string schema_string) {

    schema_vec schema;
    vector<std::string> elems;

    // schema col info may be delimited by either ; or newline, currently
    if (schema_string.find(';') != std::string::npos) {
        boost::split(elems, schema_string, boost::is_any_of(";"),
                     boost::token_compress_on);
    }
    else if (schema_string.find('\n') != std::string::npos) {
        boost::split(elems, schema_string, boost::is_any_of("\n"),
                     boost::token_compress_on);
    }
    else {
        assert (TablesErrCodes::BadDataSchemaFormat==0);
    }

    // assume schema string contains at least one col's info
    if (elems.size() < 1)
        assert (TablesErrCodes::EmptySchema==0);

    for (auto it = elems.begin(); it != elems.end(); ++it) {

        vector<std::string> col_data;  // each string describes one col info
        std::string col_info_string = *it;
        boost::trim(col_info_string);

        // expected num of metadata items in our Tables::col_info struct
        uint32_t col_metadata_items = NUM_COL_INFO_FIELDS;

        // ignore empty strings after trimming, due to above boost split.
        // expected len of at least n items with n-1 spaces
        uint32_t col_info_string_min_len = (2 * col_metadata_items) - 1;
        if (col_info_string.length() < col_info_string_min_len)
            continue;

        boost::split(col_data, col_info_string, boost::is_any_of(" "),
                     boost::token_compress_on);

        if (col_data.size() != col_metadata_items)
            assert (TablesErrCodes::BadColInfoFormat==0);

        std::string name = col_data[4];
        boost::trim(name);
        const struct col_info ci(col_data[0], col_data[1], col_data[2],
                                 col_data[3], name);
        schema.push_back(ci);
    }
    return schema;
}

predicate_vec predsFromString(schema_vec &schema, std::string preds_string) {
    // format:  ;colname,opname,value;colname,opname,value;...
    // e.g., ;orderkey,eq,5;comment,like,hello world;..

    predicate_vec preds;
    boost::trim(preds_string);  // whitespace
    boost::trim_if(preds_string, boost::is_any_of(PRED_DELIM_OUTER));

    if (preds_string.empty() || preds_string== SELECT_DEFAULT) return preds;

    vector<std::string> pred_items;
    boost::split(pred_items, preds_string, boost::is_any_of(PRED_DELIM_OUTER),
                 boost::token_compress_on);
    vector<std::string> colnames;
    vector<std::string> select_descr;

    Tables::predicate_vec agg_preds;
    for (auto it=pred_items.begin(); it!=pred_items.end(); ++it) {
        boost::split(select_descr, *it, boost::is_any_of(PRED_DELIM_INNER),
                     boost::token_compress_on);

        assert(select_descr.size()==3);  // currently a triple per pred.

        std::string colname = select_descr.at(0);
        std::string opname = select_descr.at(1);
        std::string val = select_descr.at(2);
        boost::to_upper(colname);

        // this only has 1 col and only used to verify input
        schema_vec sv = schemaFromColNames(schema, colname);
        if (sv.empty()) {
            cerr << "Error: colname=" << colname << " not present in schema."
                 << std::endl;
            assert (TablesErrCodes::RequestedColNotPresent == 0);
        }
        col_info ci = sv.at(0);
        int op_type = skyOpTypeFromString(opname);

        switch (ci.type) {

            case SDT_BOOL: {
                TypedPredicate<bool>* p = \
                        new TypedPredicate<bool> \
                        (ci.idx, ci.type, op_type, std::stol(val));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_INT8: {
                TypedPredicate<int8_t>* p = \
                        new TypedPredicate<int8_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<int8_t>(std::stol(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_INT16: {
                TypedPredicate<int16_t>* p = \
                        new TypedPredicate<int16_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<int16_t>(std::stol(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_INT32: {
                TypedPredicate<int32_t>* p = \
                        new TypedPredicate<int32_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<int32_t>(std::stol(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_INT64: {
                TypedPredicate<int64_t>* p = \
                        new TypedPredicate<int64_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<int64_t>(std::stoll(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_UINT8: {
                TypedPredicate<uint8_t>* p = \
                        new TypedPredicate<uint8_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<uint8_t>(std::stoul(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_UINT16: {
                TypedPredicate<uint16_t>* p = \
                        new TypedPredicate<uint16_t> \
                        (ci.idx, ci.type, op_type,
                        static_cast<uint16_t>(std::stoul(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_UINT32: {
                TypedPredicate<uint32_t>* p = \
                        new TypedPredicate<uint32_t> \
                        (ci.idx, ci.type, op_type,
                        static_cast<uint32_t>(std::stoul(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_UINT64: {
                TypedPredicate<uint64_t>* p = \
                        new TypedPredicate<uint64_t> \
                        (ci.idx, ci.type, op_type, \
                        static_cast<uint64_t>(std::stoull(val)));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_FLOAT: {
                TypedPredicate<float>* p = \
                        new TypedPredicate<float> \
                        (ci.idx, ci.type, op_type, std::stof(val));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_DOUBLE: {
                TypedPredicate<double>* p = \
                        new TypedPredicate<double> \
                        (ci.idx, ci.type, op_type, std::stod(val));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_CHAR: {
                TypedPredicate<char>* p = \
                        new TypedPredicate<char> \
                        (ci.idx, ci.type, op_type, std::stol(val));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_UCHAR: {
                TypedPredicate<unsigned char>* p = \
                        new TypedPredicate<unsigned char> \
                        (ci.idx, ci.type, op_type, std::stoul(val));
                if (p->isGlobalAgg()) agg_preds.push_back(p);
                else preds.push_back(p);
                break;
            }
            case SDT_STRING: {
                TypedPredicate<std::string>* p = \
                        new TypedPredicate<std::string> \
                        (ci.idx, ci.type, op_type, val);
                preds.push_back(p);
                break;
            }
            case SDT_DATE: {
                TypedPredicate<std::string>* p = \
                        new TypedPredicate<std::string> \
                        (ci.idx, ci.type, op_type, val);
                preds.push_back(p);
                break;
            }
            default: assert (TablesErrCodes::UnknownSkyDataType==0);
        }
    }

    // add agg preds to end so they are only updated if all other preds pass.
    // currently in apply_predicates they are applied in order.
    if (!agg_preds.empty()) {
        preds.reserve(preds.size() + agg_preds.size());
        std::move(agg_preds.begin(), agg_preds.end(),
                  std::inserter(preds, preds.end()));
        agg_preds.clear();
        agg_preds.shrink_to_fit();
    }
    return preds;
}

std::vector<std::string> colnamesFromPreds(predicate_vec &preds,
                                           schema_vec &schema) {
    std::vector<std::string> colnames;
    for (auto it_prd=preds.begin(); it_prd!=preds.end(); ++it_prd) {
        for (auto it_scm=schema.begin(); it_scm!=schema.end(); ++it_scm) {
            if ((*it_prd)->colIdx() == it_scm->idx) {
                colnames.push_back(it_scm->name);
            }
        }
    }
    return colnames;
}

std::vector<std::string> colnamesFromSchema(schema_vec &schema) {
    std::vector<std::string> colnames;
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        colnames.push_back(it->name);
    }
    return colnames;
}

std::string predsToString(predicate_vec &preds, schema_vec &schema) {
    // output format:  "|orderkey,lt,5|comment,like,he|extendedprice,gt,2.01|"
    // where '|' and ',' are denoted as PRED_DELIM_OUTER and PRED_DELIM_INNER

    std::string preds_str;

    // for each pred specified, we iterate over the schema to find its
    // correpsonding column index so we can build the col value string
    // based on col type.
    for (auto it_prd = preds.begin(); it_prd != preds.end(); ++it_prd) {
        for (auto it_sch = schema.begin(); it_sch != schema.end(); ++it_sch) {
            col_info ci = *it_sch;

            // if col indexes match then build the value string.
            if (((*it_prd)->colIdx() == ci.idx) or
                ((*it_prd)->colIdx() == RID_COL_INDEX)) {
                preds_str.append(PRED_DELIM_OUTER);
                std::string colname;

                // set the column name string
                if ((*it_prd)->colIdx() == RID_COL_INDEX)
                    colname = RID_INDEX;  // special col index for RID 'col'
                else
                    colname = ci.name;
                preds_str.append(colname);
                preds_str.append(PRED_DELIM_INNER);
                preds_str.append(skyOpTypeToString((*it_prd)->opType()));
                preds_str.append(PRED_DELIM_INNER);

                // set the col's value as string based on data type
                std::string val;
                switch ((*it_prd)->colType()) {

                    case SDT_BOOL: {
                        TypedPredicate<bool>* p = \
                            dynamic_cast<TypedPredicate<bool>*>(*it_prd);
                        val = std::string(1, p->Val());
                        break;
                    }
                    case SDT_INT8: {
                        TypedPredicate<int8_t>* p = \
                            dynamic_cast<TypedPredicate<int8_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_INT16: {
                        TypedPredicate<int16_t>* p = \
                            dynamic_cast<TypedPredicate<int16_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_INT32: {
                        TypedPredicate<int32_t>* p = \
                            dynamic_cast<TypedPredicate<int32_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_INT64: {
                        TypedPredicate<int64_t>* p = \
                            dynamic_cast<TypedPredicate<int64_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_UINT8: {
                        TypedPredicate<uint8_t>* p = \
                            dynamic_cast<TypedPredicate<uint8_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_UINT16: {
                        TypedPredicate<uint16_t>* p = \
                            dynamic_cast<TypedPredicate<uint16_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_UINT32: {
                        TypedPredicate<uint32_t>* p = \
                            dynamic_cast<TypedPredicate<uint32_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_UINT64: {
                        TypedPredicate<uint64_t>* p = \
                            dynamic_cast<TypedPredicate<uint64_t>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_CHAR: {
                        TypedPredicate<char>* p = \
                            dynamic_cast<TypedPredicate<char>*>(*it_prd);
                        val = std::string(1, p->Val());
                        break;
                    }
                    case SDT_UCHAR: {
                        TypedPredicate<unsigned char>* p = \
                            dynamic_cast<TypedPredicate<unsigned char>*>(*it_prd);
                        val = std::string(1, p->Val());
                        break;
                    }
                    case SDT_FLOAT: {
                        TypedPredicate<float>* p = \
                            dynamic_cast<TypedPredicate<float>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_DOUBLE: {
                        TypedPredicate<double>* p = \
                            dynamic_cast<TypedPredicate<double>*>(*it_prd);
                        val = std::to_string(p->Val());
                        break;
                    }
                    case SDT_STRING:
                    case SDT_DATE: {
                        TypedPredicate<std::string>* p = \
                            dynamic_cast<TypedPredicate<std::string>*>(*it_prd);
                        val = p->Val();
                        break;
                    }
                    default: assert (!val.empty());
                }
                preds_str.append(val);
            }
            if ((*it_prd)->colIdx() == RID_COL_INDEX)
                break;  // only 1 RID col in the schema
        }
    }
    preds_str.append(PRED_DELIM_OUTER);
    return preds_str;
}

int skyOpTypeFromString(std::string op) {
    int op_type = 0;
    if (op=="lt") op_type = SOT_lt;
    else if (op=="gt") op_type = SOT_gt;
    else if (op=="eq") op_type = SOT_eq;
    else if (op=="ne") op_type = SOT_ne;
    else if (op=="leq") op_type = SOT_leq;
    else if (op=="geq") op_type = SOT_geq;
    else if (op=="add") op_type = SOT_add;
    else if (op=="sub") op_type = SOT_sub;
    else if (op=="mul") op_type = SOT_mul;
    else if (op=="div") op_type = SOT_div;
    else if (op=="min") op_type = SOT_min;
    else if (op=="max") op_type = SOT_max;
    else if (op=="sum") op_type = SOT_sum;
    else if (op=="cnt") op_type = SOT_cnt;
    else if (op=="like") op_type = SOT_like;
    else if (op=="in") op_type = SOT_in;
    else if (op=="not_in") op_type = SOT_not_in;
    else if (op=="before") op_type = SOT_before;
    else if (op=="between") op_type = SOT_between;
    else if (op=="after") op_type = SOT_after;
    else if (op=="logical_or") op_type = SOT_logical_or;
    else if (op=="logical_and") op_type = SOT_logical_and;
    else if (op=="logical_not") op_type = SOT_logical_not;
    else if (op=="logical_nor") op_type = SOT_logical_nor;
    else if (op=="logical_xor") op_type = SOT_logical_xor;
    else if (op=="logical_nand") op_type = SOT_logical_nand;
    else if (op=="bitwise_and") op_type = SOT_bitwise_and;
    else if (op=="bitwise_or") op_type = SOT_bitwise_or;
    else assert (TablesErrCodes::OpNotRecognized==0);
    return op_type;
}

std::string skyOpTypeToString(int op) {
    std::string op_str;
    if (op==SOT_lt) op_str = "lt";
    else if (op==SOT_gt) op_str = "gt";
    else if (op==SOT_eq) op_str = "eq";
    else if (op==SOT_ne) op_str = "ne";
    else if (op==SOT_leq) op_str = "leq";
    else if (op==SOT_geq) op_str = "geq";
    else if (op==SOT_add) op_str = "add";
    else if (op==SOT_sub) op_str = "sub";
    else if (op==SOT_mul) op_str = "mul";
    else if (op==SOT_div) op_str = "div";
    else if (op==SOT_min) op_str = "min";
    else if (op==SOT_max) op_str = "max";
    else if (op==SOT_sum) op_str = "sum";
    else if (op==SOT_cnt) op_str = "cnt";
    else if (op==SOT_like) op_str = "like";
    else if (op==SOT_in) op_str = "in";
    else if (op==SOT_not_in) op_str = "not_in";
    else if (op==SOT_before) op_str = "before";
    else if (op==SOT_between) op_str = "between";
    else if (op==SOT_after) op_str = "after";
    else if (op==SOT_logical_or) op_str = "logical_or";
    else if (op==SOT_logical_and) op_str = "logical_and";
    else if (op==SOT_logical_not) op_str = "logical_not";
    else if (op==SOT_logical_nor) op_str = "logical_nor";
    else if (op==SOT_logical_xor) op_str = "logical_xor";
    else if (op==SOT_logical_nand) op_str = "logical_nand";
    else if (op==SOT_bitwise_and) op_str = "bitwise_and";
    else if (op==SOT_bitwise_or) op_str = "bitwise_or";
    else assert (!op_str.empty());
    return op_str;
}

void printSkyRootHeader(sky_root &r) {

    std::cout << "\n\nSKYHOOK_ROOT HEADER"<< std::endl;
    std::cout << "skyhook_version: "<< r.skyhook_version << std::endl;
    std::cout << "data_format_type: "<< r.data_format_type << std::endl;
    std::cout << "data_structure_version: "<< r.data_structure_version << std::endl;
    std::cout << "data_schema_version: "<< r.data_schema_version << std::endl;
    std::cout << "db_schema_name: "<< r.db_schema_name << std::endl;
    std::cout << "table name: "<< r.table_name << std::endl;
    std::cout << "data_schema: \n"<< r.data_schema << std::endl;
    std::cout << "delete vector: [";
        for (int i=0; i< (int)r.delete_vec.size(); i++) {
            std::cout << (int)r.delete_vec[i];
            if (i != (int)r.delete_vec.size()-1)
                std::cout <<", ";
        }
    std::cout << "]" << std::endl;
    std::cout << "nrows: " << r.nrows << std::endl;
    std::cout << std::endl;
}

void printSkyRecHeader(sky_rec &r) {

    std::cout << "\nSKYHOOK_REC HEADER" << std::endl;
    std::cout << "RID: "<< r.RID << std::endl;
    std::string bitstring = "";
    int64_t val = 0;
    uint64_t bit = 0;
    for(int j = 0; j < (int)r.nullbits.size(); j++) {
        val = r.nullbits.at(j);
        for (uint64_t k=0; k < 8 * sizeof(r.nullbits.at(j)); k++) {
            uint64_t mask =  1 << k;
            ((val&mask)>0) ? bit=1 : bit=0;
            bitstring.append(std::to_string(bit));
        }
        std::cout << "nullbits ["<< j << "]: val=" << val << ": bits="
                  << bitstring;
        std::cout << std::endl;
        bitstring.clear();
    }
}

void printSkyRecHeader_fbu(sky_rec_fbu &r) {

    std::cout << "\nSKYHOOK_REC_FBU HEADER" << std::endl;
    std::cout << "RID: "<< r.RID << std::endl;
    std::string bitstring = "";
    int64_t val = 0;
    uint64_t bit = 0;
    for(int j = 0; j < (int)r.nullbits.size(); j++) {
        val = r.nullbits.at(j);
        for (uint64_t k=0; k < 8 * sizeof(r.nullbits.at(j)); k++) {
            uint64_t mask =  1 << k;
            ((val&mask)>0) ? bit=1 : bit=0;
            bitstring.append(std::to_string(bit));
        }
        std::cout << "nullbits ["<< j << "]: val=" << val << ": bits="
                  << bitstring;
        std::cout << std::endl;
        bitstring.clear();
    }
}

void printSkyColHeader_fbu(sky_col_fbu &c) {

    std::cout << "\nSKYHOOK_COL HEADER" << std::endl;
    std::cout << "CID: "<< c.CID << std::endl;
    std::string bitstring = "";
    int64_t val = 0;
    uint64_t bit = 0;
    for(int j = 0; j < (int)c.nullbits.size(); j++) {
        val = c.nullbits.at(j);
        for (uint64_t k=0; k < 8 * sizeof(c.nullbits.at(j)); k++) {
            uint64_t mask =  1 << k;
            ((val&mask)>0) ? bit=1 : bit=0;
            bitstring.append(std::to_string(bit));
        }
        std::cout << "nullbits ["<< j << "]: val=" << val << ": bits="
                  << bitstring;
        std::cout << std::endl;
        bitstring.clear();
    }
}

long long int printFlatbufFlexRowAsCsv(
        const char* dataptr,
        const size_t datasz,
        bool print_header,
        bool print_verbose,
        long long int max_to_print) {

    // get root table ptr as sky struct
    sky_root skyroot = getSkyRoot(dataptr, datasz);
    schema_vec sc = schemaFromString(skyroot.data_schema);
    assert(!sc.empty());

    if (print_verbose)
        printSkyRootHeader(skyroot);

    // print header row showing schema
    if (print_header) {
        bool first = true;
        for (schema_vec::iterator it = sc.begin(); it != sc.end(); ++it) {
            if (!first) std::cout << CSV_DELIM;
            first = false;
            std::cout << it->name;
            if (it->is_key) std::cout << "(key)";
            if (!it->nullable) std::cout << "(NOT NULL)";

        }
        std::cout << std::endl; // newline to start first row.
    }

    long long int counter = 0;
    for (uint32_t i = 0; i < skyroot.nrows; i++, counter++) {
        if (counter >= max_to_print)
            break;

        if (skyroot.delete_vec.at(i) == 1) continue;  // skip dead rows.

        // get the record struct, then the row data
        sky_rec skyrec = getSkyRec(static_cast<row_offs>(skyroot.data_vec)->Get(i));
        auto row = skyrec.data.AsVector();

        if (print_verbose)
            printSkyRecHeader(skyrec);

        // for each col in the row, print a NULL or the col's value/
        bool first = true;
        for (uint32_t j = 0; j < sc.size(); j++ ) {
            if (!first) std::cout << CSV_DELIM;
            first = false;
            col_info col = sc.at(j);

            if (col.nullable) {  // check nullbit
                bool is_null = false;
                int pos = col.idx / (8*sizeof(skyrec.nullbits.at(0)));
                int col_bitmask = 1 << col.idx;
                if ((col_bitmask & skyrec.nullbits.at(pos)) != 0)  {
                    is_null =true;
                }
                if (is_null) {
                    std::cout << "NULL";
                    continue;
                }
            }
            switch (col.type) {
                case SDT_BOOL: std::cout << row[j].AsBool(); break;
                case SDT_INT8: std::cout << row[j].AsInt8(); break;
                case SDT_INT16: std::cout << row[j].AsInt16(); break;
                case SDT_INT32: std::cout << row[j].AsInt32(); break;
                case SDT_INT64: std::cout << row[j].AsInt64(); break;
                case SDT_UINT8: std::cout << row[j].AsUInt8(); break;
                case SDT_UINT16: std::cout << row[j].AsUInt16(); break;
                case SDT_UINT32: std::cout << row[j].AsUInt32(); break;
                case SDT_UINT64: std::cout << row[j].AsUInt64(); break;
                case SDT_FLOAT: std::cout << row[j].AsFloat(); break;
                case SDT_DOUBLE: std::cout << row[j].AsDouble(); break;
                case SDT_CHAR: std::cout <<
                    std::string(1, row[j].AsInt8()); break;
                case SDT_UCHAR: std::cout <<
                    std::string(1, row[j].AsUInt8()); break;
                case SDT_DATE: std::cout <<
                    row[j].AsString().str(); break;
                case SDT_STRING: std::cout <<
                    row[j].AsString().str(); break;
                default: assert (TablesErrCodes::UnknownSkyDataType);
            }
        }
        std::cout << std::endl;  // newline to start next row.
    }
    return counter;
}

long long int printFlatbufFBUAsCsv(
        const char* dataptr,
        const size_t datasz,
        bool print_header,
        bool print_verbose,
        long long int max_to_print,
        SkyFormatType format ) {

    sky_root skyroot = getSkyRoot( dataptr, datasz, format ) ;
    schema_vec sc    = schemaFromString( skyroot.data_schema ) ;
    assert( !sc.empty() ) ;

    if (print_verbose)
        printSkyRootHeader( skyroot ) ;

    // print header row showing schema
    if (print_header) {
        bool first = true;
        for (schema_vec::iterator it = sc.begin(); it != sc.end(); ++it) {
            if (!first) std::cout << CSV_DELIM;
            first = false;
            std::cout << it->name;
            if (it->is_key) std::cout << "(key)";
            if (!it->nullable) std::cout << "(NOT NULL)";
        }
        std::cout << std::endl; // newline to start first row.
    }

    switch( skyroot.data_format_type ) {

        case SFT_FLATBUF_UNION_ROW : {
            long long int counter = 0;
            for (uint32_t i = 0; i < skyroot.nrows; i++, counter++) {
                if (counter >= max_to_print)
                    break;

                if (skyroot.delete_vec.at(i) == 1) continue;  // skip dead rows.

                // get the record struct, then the row data
                sky_rec_fbu skyrec = getSkyRec_fbu( skyroot, i ) ;
                if (print_verbose)
                    printSkyRecHeader_fbu(skyrec);

                // -------------------------------------------------------------------- //
                auto curr_rec_data = skyrec.data_fbu_rows ;
                // for each col in the row, print a NULL or the col's value/
                bool first = true;
                for( unsigned int j = 0; j < sc.size(); j++ ) {
                    if (!first) std::cout << CSV_DELIM;
                    first = false;
                    col_info col = sc.at(j);

                    if (col.nullable) {  // check nullbit
                        bool is_null = false;
                        int pos = col.idx / (8*sizeof(skyrec.nullbits.at(0)));
                        int col_bitmask = 1 << col.idx;
                        if ((col_bitmask & skyrec.nullbits.at(pos)) != 0)  {
                            is_null =true;
                        }
                        if (is_null) {
                            std::cout << "NULL";
                            continue;
                        }
                    }
                    switch( col.type ) {
                      case SDT_UINT64 : {
                        auto int_col_data = 
                            static_cast< const Tables::SDT_UINT64_FBU* >( curr_rec_data->Get(j) ) ;
                        std::cout << int_col_data->data()->Get(0) ;
                        break ;
                      }
                      case SDT_FLOAT : {
                        auto float_col_data = 
                            static_cast< const Tables::SDT_FLOAT_FBU* >( curr_rec_data->Get(j) ) ;
                        std::cout << float_col_data->data()->Get(0) ;
                        break ;
                      }
                      case SDT_STRING : {
                        auto string_col_data = 
                            static_cast< const Tables::SDT_STRING_FBU* >( curr_rec_data->Get(j) ) ;
                        std::cout << string_col_data->data()->Get(0)->str() ;
                        break ;
                      }
                      default :
                        assert (TablesErrCodes::UnknownSkyDataType==0);
                    } //switch
                } //for loop
                std::cout << std::endl ;
            } //for
            return counter;
            break ;
        } //Rows
        // -------------------------------------------------------------------- //

        case SFT_FLATBUF_UNION_COL : {

            long long int counter = 0;
            bool first = true;
            // iterate over rows
            for( unsigned int j = 0; j < skyroot.nrows; j++ ) {
                if (!first) std::cout << CSV_DELIM;
                first = false;

                // iterate over columns
                for( unsigned int i = 0; i < sc.size(); i++ ) {
                    if (counter >= max_to_print)
                        break;

                    if (skyroot.delete_vec.at(i) == 1) continue;  // skip dead rows.
                    col_info col = sc.at(i);

                    // get the record struct, then the row data
                    sky_col_fbu skycol = getSkyCol_fbu( skyroot, i ) ;
                    if (print_verbose)
                        printSkyColHeader_fbu(skycol);
  
                    auto this_col       = skycol.data_fbu_col ;
                    //auto this_col_name  = this_col->col_name() ;
                    //auto this_col_index = this_col->col_index() ;
                    auto curr_col_data  = this_col->data() ;

                    if (col.nullable) {  // check nullbit
                        bool is_null = false;
                        int pos = col.idx / (8*sizeof(skycol.nullbits.at(0)));
                        int col_bitmask = 1 << col.idx;
                        if ((col_bitmask & skycol.nullbits.at(pos)) != 0)  {
                            is_null =true;
                        }
                        if (is_null) {
                            std::cout << "NULL";
                            continue;
                        }
                    }

                    switch( col.type ) {
                      case SDT_UINT64 : {
                          auto column_of_data = 
                              static_cast< const Tables::SDT_UINT64_FBU* >( curr_col_data ) ;
                          auto data_at_row = column_of_data->data()->Get(j) ;
                          std::cout << std::to_string( data_at_row ) << std::endl ;
                          break ;
                      }
                      case SDT_FLOAT : {
                          auto column_of_data = 
                              static_cast< const Tables::SDT_FLOAT_FBU* >( curr_col_data ) ;
                          auto data_at_row = column_of_data->data()->Get(j) ;
                          std::cout << std::to_string( data_at_row ) << std::endl ;
                          break ;
                      }
                      case SDT_STRING : {
                          auto column_of_data = 
                              static_cast< const Tables::SDT_STRING_FBU* >( curr_col_data ) ;
                          auto data_at_row = column_of_data->data()->Get(j)->str() ;
                          std::cout << data_at_row << std::endl ;
                          break ;
                      }
                      default :
                          assert (TablesErrCodes::UnknownSkyDataType==0);
                    } //switch
                } //for
              std::cout << "looping..." << std::endl ;
            } //for
            return counter ;
            break ;
        }
        default:
            assert( SkyFormatTypeNotRecognized==0 ) ;

    } //switch

    return -1 ;
}

// Highest level abstraction over our data on disk.
// Wraps a supported format (flatbuf, arrow, csv, parquet,...)
// along with its metadata.  This unified structure is used as the primary
// store/send/retreive data structure for many supported formats

sky_meta getSkyMeta(bufferlist bl, bool is_meta, int data_format) {

    if (is_meta) {
        // get data as contiguous bytes before accessing
        const FB_Meta* meta = GetFB_Meta(bl.c_str());
        return sky_meta(
            meta->format_type(),      // the blob's format (i.e.,SkyFormatType)
            meta->is_deleted(),       // is this blob still valid (not deleted)
            meta->data()->size(),     // blob actual size
            meta->global_off(),       // data position in original file
            meta->len(),              // data len in original file
            meta->compression_type(), // blob compression
            reinterpret_cast<const char*>(meta->data()->Data()));  // data blob
    }
    else {
        return sky_meta(    // for testing new raw formats without meta wrapper
            data_format,
            false,
            bl.length(),
            0,              // unused
            0,              // unused
            none,           // no compression
            bl.c_str());    // get data as contiguous bytes before accessing
    }
}

sky_root getSkyRoot(const char *ds, size_t ds_size, int ds_format) {

    int skyhook_version;
    int data_format_type;
    int data_structure_version;
    int data_schema_version;
    std::string data_schema;
    std::string db_schema_name;
    std::string table_name;
    delete_vector delete_vec;
    row_offs data_vec;
    row_offs_fbu_rows data_vec_fbu_rows;
    cols_fbu cols ;
    uint32_t nrows;

    switch (ds_format) {

        case SFT_FLATBUF_FLEX_ROW: {
            const Table* root = GetTable(ds);
            skyhook_version = root->skyhook_version();
            data_format_type = root->data_format_type();
            data_structure_version = root->data_structure_version();
            data_schema_version = root->data_schema_version();
            data_schema = root->data_schema()->str();
            db_schema_name = root->db_schema()->str();
            table_name = root->table_name()->str();
            delete_vec = delete_vector(root->delete_vector()->begin(),
                                       root->delete_vector()->end());
            data_vec = root->rows();
            nrows = root->nrows();
            break;
        }

        case SFT_FLATBUF_UNION_ROW:
        case SFT_FLATBUF_UNION_COL: {
            auto root = Tables::GetRoot_FBU( ds ) ;
            delete_vec = delete_vector( root->delete_vector()->begin(),
                                        root->delete_vector()->end() ) ;

            skyhook_version        = root->skyhook_version() ;
            data_format_type       = root->data_format_type() ;
            data_structure_version = root->data_structure_version() ;
            data_schema_version    = root->data_schema_version() ;
            data_schema            = root->data_schema()->str() ;
            db_schema_name         = root->db_schema_name()->str() ;
            table_name             = root->table_name()->str() ;
            nrows                  = root->nrows() ;

            if( ds_format == SFT_FLATBUF_UNION_ROW ) {
              auto rows = static_cast< const Tables::Rows_FBU* >( root->relationData() ) ;
              data_vec_fbu_rows = rows->data() ;
              return sky_root(
                  skyhook_version,
                  data_format_type,
                  data_structure_version,
                  data_schema_version,
                  data_schema,
                  db_schema_name,
                  table_name,
                  delete_vec,
                  data_vec_fbu_rows,
                  nrows
              );
            }
            else if( ds_format == SFT_FLATBUF_UNION_COL ) {
              cols = static_cast< const Tables::Cols_FBU* >( root->relationData() ) ;
              return sky_root(
                  skyhook_version,
                  data_format_type,
                  data_structure_version,
                  data_schema_version,
                  data_schema,
                  db_schema_name,
                  table_name,
                  delete_vec,
                  cols,
                  nrows
              );
            }
            else
                assert (SkyFormatTypeNotRecognized==0);
            break ;
        }
        case SFT_ARROW:
        case SFT_FLATBUF_CSV_ROW:
        case SFT_PG_TUPLE:
        case SFT_CSV:
        default:
            assert (SkyFormatTypeNotRecognized==0);
    }

    return sky_root(
        skyhook_version,
        data_format_type,
        data_structure_version,
        data_schema_version,
        data_schema,
        db_schema_name,
        table_name,
        delete_vec,
        data_vec,
        nrows
    );
}

sky_rec getSkyRec(const Tables::Record* rec) {

    return sky_rec(
        rec->RID(),
        nullbits_vector(rec->nullbits()->begin(), rec->nullbits()->end()),
        rec->data_flexbuffer_root()
    );
}

sky_rec_fbu getSkyRec_fbu( sky_root root, int recid ) {
    switch( root.data_format_type ) {
        case SFT_FLATBUF_UNION_ROW : {
          const Tables::Record_FBU* rec = static_cast< row_offs_fbu_rows >(root.data_vec)->Get( recid ) ;
          return sky_rec_fbu(
            recid,
            nullbits_vector( rec->nullbits()->begin(), rec->nullbits()->end() ),
            rec->data() //row_data_ref_fbu_rows
          );
          break ;
        }
        default:
            assert (SkyFormatTypeNotRecognized==0);
    } //switch
} //getSkyRec_fbu

sky_col_fbu getSkyCol_fbu( sky_root root, int colid ) {
    switch( root.data_format_type ) {
        case SFT_FLATBUF_UNION_COL : {
            auto cols = static_cast< const Tables::Cols_FBU* >( root.data_vec ) ;
            auto cols_data = cols->data() ;
            auto col_at_index = cols_data->Get( colid ) ;
            //auto col_name = col_at_index->col_name() ;
            //auto col_index = col_at_index->col_index() ;
            auto col_nullbits = col_at_index->nullbits() ;
            auto a = col_nullbits->begin() ;
            auto b = col_nullbits->end() ;
            auto null_vec = nullbits_vector( a, b ) ;
            return sky_col_fbu(
              colid,
              null_vec,
              col_at_index
            );
          break ;
        }
        default:
            assert (SkyFormatTypeNotRecognized==0);
    } //switch
} //getSkyCol_fbu

bool hasAggPreds(predicate_vec &preds) {
    for (auto it=preds.begin(); it!=preds.end();++it)
        if ((*it)->isGlobalAgg()) return true;
    return false;
}

bool applyPredicates(predicate_vec& pv, sky_rec& rec) {

    bool rowpass = false;
    bool init_rowpass = false;
    auto row = rec.data.AsVector();

    for (auto it = pv.begin(); it != pv.end(); ++it) {

        int chain_optype = (*it)->chainOpType();

        if (!init_rowpass) {
            if (chain_optype == SOT_logical_or)
                rowpass = false;
            else if (chain_optype == SOT_logical_and)
                rowpass = true;
            else
                rowpass = true;  // default to logical AND
            init_rowpass = true;
        }

        if ((chain_optype == SOT_logical_and) and !rowpass) break;

        bool colpass = false;
        switch((*it)->colType()) {

            // NOTE: predicates have typed ints but our int comparison
            // functions are defined on 64bit ints.
            case SDT_BOOL: {
                TypedPredicate<bool>* p = \
                        dynamic_cast<TypedPredicate<bool>*>(*it);
                bool colval = row[p->colIdx()].AsBool();
                bool predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            case SDT_INT8: {
                TypedPredicate<int8_t>* p = \
                        dynamic_cast<TypedPredicate<int8_t>*>(*it);
                int8_t colval = row[p->colIdx()].AsInt8();
                int8_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_INT16: {
                TypedPredicate<int16_t>* p = \
                        dynamic_cast<TypedPredicate<int16_t>*>(*it);
                int16_t colval = row[p->colIdx()].AsInt16();
                int16_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_INT32: {
                TypedPredicate<int32_t>* p = \
                        dynamic_cast<TypedPredicate<int32_t>*>(*it);
                int32_t colval = row[p->colIdx()].AsInt32();
                int32_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_INT64: {
                TypedPredicate<int64_t>* p = \
                        dynamic_cast<TypedPredicate<int64_t>*>(*it);
                int64_t colval = 0;
                if ((*it)->colIdx() == RID_COL_INDEX)
                    colval = rec.RID;  // RID val not in the row
                else
                    colval = row[p->colIdx()].AsInt64();
                int64_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            case SDT_UINT8: {
                TypedPredicate<uint8_t>* p = \
                        dynamic_cast<TypedPredicate<uint8_t>*>(*it);
                uint8_t colval = row[p->colIdx()].AsUInt8();
                uint8_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_UINT16: {
                TypedPredicate<uint16_t>* p = \
                        dynamic_cast<TypedPredicate<uint16_t>*>(*it);
                uint16_t colval = row[p->colIdx()].AsUInt16();
                uint16_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_UINT32: {
                TypedPredicate<uint32_t>* p = \
                        dynamic_cast<TypedPredicate<uint32_t>*>(*it);
                uint32_t colval = row[p->colIdx()].AsUInt32();
                uint32_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }

            case SDT_UINT64: {
                TypedPredicate<uint64_t>* p = \
                        dynamic_cast<TypedPredicate<uint64_t>*>(*it);
                uint64_t colval = 0;
                if ((*it)->colIdx() == RID_COL_INDEX) // RID val not in the row
                    colval = rec.RID;
                else
                    colval = row[p->colIdx()].AsUInt64();
                uint64_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            case SDT_FLOAT: {
                TypedPredicate<float>* p = \
                        dynamic_cast<TypedPredicate<float>*>(*it);
                float colval = row[p->colIdx()].AsFloat();
                float predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<double>(predval),
                                      p->opType());
                break;
            }

            case SDT_DOUBLE: {
                TypedPredicate<double>* p = \
                        dynamic_cast<TypedPredicate<double>*>(*it);
                double colval = row[p->colIdx()].AsDouble();
                double predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            case SDT_CHAR: {
                TypedPredicate<char>* p= \
                        dynamic_cast<TypedPredicate<char>*>(*it);
                if (p->opType() == SOT_like) {
                    // use strings for regex
                    std::string colval = row[p->colIdx()].AsString().str();
                    std::string predval = std::to_string(p->Val());
                    colpass = compare(colval,predval,p->opType(),p->colType());
                }
                else {
                    // use int val comparision method
                    int8_t colval = row[p->colIdx()].AsInt8();
                    int8_t predval = p->Val();
                    if (p->isGlobalAgg())
                        p->updateAgg(computeAgg(colval,predval,p->opType()));
                    else
                        colpass = compare(colval,
                                          static_cast<int64_t>(predval),
                                          p->opType());
                }
                break;
            }

            case SDT_UCHAR: {
                TypedPredicate<unsigned char>* p = \
                        dynamic_cast<TypedPredicate<unsigned char>*>(*it);
                if (p->opType() == SOT_like) {
                    // use strings for regex
                    std::string colval = row[p->colIdx()].AsString().str();
                    std::string predval = std::to_string(p->Val());
                    colpass = compare(colval,predval,p->opType(),p->colType());
                }
                else {
                    // use int val comparision method
                    uint8_t colval = row[p->colIdx()].AsUInt8();
                    uint8_t predval = p->Val();
                    if (p->isGlobalAgg())
                        p->updateAgg(computeAgg(colval,predval,p->opType()));
                    else
                        colpass = compare(colval,
                                          static_cast<uint64_t>(predval),
                                          p->opType());
                }
                break;
            }

            case SDT_STRING:
            case SDT_DATE: {
                TypedPredicate<std::string>* p = \
                        dynamic_cast<TypedPredicate<std::string>*>(*it);
                string colval = row[p->colIdx()].AsString().str();
                colpass = compare(colval,p->Val(),p->opType(),p->colType());
                break;
            }

            default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
        }

        // incorporate local col passing into the decision to pass row.
        switch (chain_optype) {
            case SOT_logical_or:
                rowpass |= colpass;
                break;
            case SOT_logical_and:
                rowpass &= colpass;
                break;
            default: // should not be reachable
                rowpass &= colpass;
        }
    }
    return rowpass;
}

bool applyPredicates_fbu_row(predicate_vec& pv, sky_rec_fbu& rec) {

    bool rowpass = false;
    bool init_rowpass = false;
    auto row = rec.data_fbu_rows ;

    for (auto it = pv.begin(); it != pv.end(); ++it) {

        int chain_optype = (*it)->chainOpType();

        if (!init_rowpass) {
            if (chain_optype == SOT_logical_or)
                rowpass = false;
            else if (chain_optype == SOT_logical_and)
                rowpass = true;
            else
                rowpass = true;  // default to logical AND
            init_rowpass = true;
        }

        if ((chain_optype == SOT_logical_and) and !rowpass) break;

        bool colpass = false;

        switch((*it)->colType()) {

            // NOTE: predicates have typed ints but our int comparison
            // functions are defined on 64bit ints.
/* not yet supported in fbu
            case SDT_BOOL: {
                TypedPredicate<bool>* p = \
                        dynamic_cast<TypedPredicate<bool>*>(*it);
                bool colval = row[p->colIdx()].AsBool();
                bool predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            se SDT_INT8: {
                TypedPredicate<int8_t>* p = \
                        dynamic_cast<TypedPredicate<int8_t>*>(*it);
                int8_t colval = row[p->colIdx()].AsInt8();
                int8_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            se SDT_INT16: {
                TypedPredicate<int16_t>* p = \
                        dynamic_cast<TypedPredicate<int16_t>*>(*it);
                int16_t colval = row[p->colIdx()].AsInt16();
                int16_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            se SDT_INT32: {
                TypedPredicate<int32_t>* p = \
                        dynamic_cast<TypedPredicate<int32_t>*>(*it);
                int32_t colval = row[p->colIdx()].AsInt32();
                int32_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<int64_t>(predval),
                                      p->opType());
                break;
            }

            se SDT_INT64: {
                TypedPredicate<int64_t>* p = \
                        dynamic_cast<TypedPredicate<int64_t>*>(*it);
                int64_t colval = 0;
                if ((*it)->colIdx() == RID_COL_INDEX)
                    colval = rec.RID;  // RID val not in the row
                else
                    colval = row[p->colIdx()].AsInt64();
                int64_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            se SDT_UINT8: {
                TypedPredicate<uint8_t>* p = \
                        dynamic_cast<TypedPredicate<uint8_t>*>(*it);
                uint8_t colval = row[p->colIdx()].AsUInt8();
                uint8_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }

            se SDT_UINT16: {
                TypedPredicate<uint16_t>* p = \
                        dynamic_cast<TypedPredicate<uint16_t>*>(*it);
                uint16_t colval = row[p->colIdx()].AsUInt16();
                uint16_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }

            se SDT_UINT32: {
                TypedPredicate<uint32_t>* p = \
                        dynamic_cast<TypedPredicate<uint32_t>*>(*it);
                uint32_t colval = row[p->colIdx()].AsUInt32();
                uint32_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<uint64_t>(predval),
                                      p->opType());
                break;
            }
not yet supported in fbu */

            case SDT_UINT64: {
                TypedPredicate<uint64_t>* p = \
                        dynamic_cast<TypedPredicate<uint64_t>*>(*it);
                uint64_t colval = 0;
                if ((*it)->colIdx() == RID_COL_INDEX) // RID val not in the row
                    colval = rec.RID;
                else {
                    auto col_data =
                        static_cast< const Tables::SDT_UINT64_FBU* >( row->Get(p->colIdx()) ) ;
                    colval = col_data->data()->Get(0) ;
                }
                uint64_t predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            case SDT_FLOAT: {
                TypedPredicate<float>* p = \
                        dynamic_cast<TypedPredicate<float>*>(*it);
                auto col_data =
                    static_cast< const Tables::SDT_FLOAT_FBU* >( row->Get(p->colIdx()) ) ;
                float colval = col_data->data()->Get(0) ;
                float predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,
                                      static_cast<double>(predval),
                                      p->opType());
                break;
            }

/* not yet supported in fbu
            case SDT_DOUBLE: {
                TypedPredicate<double>* p = \
                        dynamic_cast<TypedPredicate<double>*>(*it);
                double colval = row[p->colIdx()].AsDouble();
                double predval = p->Val();
                if (p->isGlobalAgg())
                    p->updateAgg(computeAgg(colval,predval,p->opType()));
                else
                    colpass = compare(colval,predval,p->opType());
                break;
            }

            se SDT_CHAR: {
                TypedPredicate<char>* p= \
                        dynamic_cast<TypedPredicate<char>*>(*it);
                if (p->opType() == SOT_like) {
                    // use strings for regex
                    std::string colval = row[p->colIdx()].AsString().str();
                    std::string predval = std::to_string(p->Val());
                    colpass = compare(colval,predval,p->opType(),p->colType());
                }
                else {
                    // use int val comparision method
                    int8_t colval = row[p->colIdx()].AsInt8();
                    int8_t predval = p->Val();
                    if (p->isGlobalAgg())
                        p->updateAgg(computeAgg(colval,predval,p->opType()));
                    else
                        colpass = compare(colval,
                                          static_cast<int64_t>(predval),
                                          p->opType());
                }
                break;
            }

            se SDT_UCHAR: {
                TypedPredicate<unsigned char>* p = \
                        dynamic_cast<TypedPredicate<unsigned char>*>(*it);
                if (p->opType() == SOT_like) {
                    // use strings for regex
                    std::string colval = row[p->colIdx()].AsString().str();
                    std::string predval = std::to_string(p->Val());
                    colpass = compare(colval,predval,p->opType(),p->colType());
                }
                else {
                    // use int val comparision method
                    uint8_t colval = row[p->colIdx()].AsUInt8();
                    uint8_t predval = p->Val();
                    if (p->isGlobalAgg())
                        p->updateAgg(computeAgg(colval,predval,p->opType()));
                    else
                        colpass = compare(colval,
                                          static_cast<uint64_t>(predval),
                                          p->opType());
                }
                break;
            }
not yet supported in fbu */
            case SDT_STRING:
            case SDT_DATE: {
                TypedPredicate<std::string>* p = \
                        dynamic_cast<TypedPredicate<std::string>*>(*it);
                auto col_data =
                    static_cast< const Tables::SDT_STRING_FBU* >( row->Get(p->colIdx()) ) ;
                string colval = col_data->data()->Get(0)->str() ;
                colpass = compare(colval,p->Val(),p->opType(),p->colType());
                break;
            }

            default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
        }

        // incorporate local col passing into the decision to pass row.
        switch (chain_optype) {
            case SOT_logical_or:
                rowpass |= colpass;
                break;
            case SOT_logical_and:
                rowpass &= colpass;
                break;
            default: // should not be reachable
                rowpass &= colpass;
        }
    } //for
    return rowpass;
}

bool compare(const int64_t& val1, const int64_t& val2, const int& op) {
    switch (op) {
        case SOT_lt: return val1 < val2;
        case SOT_gt: return val1 > val2;
        case SOT_eq: return val1 == val2;
        case SOT_ne: return val1 != val2;
        case SOT_leq: return val1 <= val2;
        case SOT_geq: return val1 >= val2;
        case SOT_logical_or: return val1 || val2;  // for predicate chaining
        case SOT_logical_and: return val1 && val2;
        case SOT_logical_not: return !val1 && !val2;  // not either, i.e., nor
        case SOT_logical_nor: return !(val1 || val2);
        case SOT_logical_nand: return !(val1 && val2);
        case SOT_logical_xor: return (val1 || val2) && (val1 != val2);
        default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
    }
    return false;  // should be unreachable
}

bool compare(const uint64_t& val1, const uint64_t& val2, const int& op) {
    switch (op) {
        case SOT_lt: return val1 < val2;
        case SOT_gt: return val1 > val2;
        case SOT_eq: return val1 == val2;
        case SOT_ne: return val1 != val2;
        case SOT_leq: return val1 <= val2;
        case SOT_geq: return val1 >= val2;
        case SOT_logical_or: return val1 || val2;  // for predicate chaining
        case SOT_logical_and: return val1 && val2;
        case SOT_logical_not: return !val1 && !val2;  // not either, i.e., nor
        case SOT_logical_nor: return !(val1 || val2);
        case SOT_logical_nand: return !(val1 && val2);
        case SOT_logical_xor: return (val1 || val2) && (val1 != val2);
        case SOT_bitwise_and: return val1 & val2;
        case SOT_bitwise_or: return val1 | val2;
        default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
    }
    return false;  // should be unreachable
}

bool compare(const double& val1, const double& val2, const int& op) {
    switch (op) {
        case SOT_lt: return val1 < val2;
        case SOT_gt: return val1 > val2;
        case SOT_eq: return val1 == val2;
        case SOT_ne: return val1 != val2;
        case SOT_leq: return val1 <= val2;
        case SOT_geq: return val1 >= val2;
        default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
    }
    return false;  // should be unreachable
}

// used for date types or regex on alphanumeric types
bool compare(const std::string& val1, const std::string& val2, const int& op, const int& data_type) {
switch(data_type){
    case SDT_DATE:{
        boost::gregorian::date d1 = boost::gregorian::from_string(val1);
        boost::gregorian::date d2 = boost::gregorian::from_string(val2);
        switch (op) {
            case SOT_before: return d1 < d2;
            case SOT_after: return d1 > d2;
            case SOT_leq: return d1 <= d2;
            case SOT_lt: return d1 < d2;
            case SOT_geq: return d1 >= d2;
            case SOT_gt: return d1 > d2;
            case SOT_eq: return d1 == d2;
            case SOT_ne: return d1 != d2;
            default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
        }
        break;
    }
    case SDT_CHAR:
    case SDT_UCHAR:
    case SDT_STRING:
            if (op == SOT_like) return RE2::PartialMatch(val1, RE2(val2));
            else assert (TablesErrCodes::PredicateComparisonNotDefined==0);
        break;
    }
    return false;  // should be unreachable
}

bool compare(const bool& val1, const bool& val2, const int& op) {
    switch (op) {
        case SOT_lt: return val1 < val2;
        case SOT_gt: return val1 > val2;
        case SOT_eq: return val1 == val2;
        case SOT_ne: return val1 != val2;
        case SOT_leq: return val1 <= val2;
        case SOT_geq: return val1 >= val2;
        case SOT_logical_or: return val1 || val2;  // for predicate chaining
        case SOT_logical_and: return val1 && val2;
        case SOT_logical_not: return !val1 && !val2;  // not either, i.e., nor
        case SOT_logical_nor: return !(val1 || val2);
        case SOT_logical_nand: return !(val1 && val2);
        case SOT_logical_xor: return (val1 || val2) && (val1 != val2);
        case SOT_bitwise_and: return val1 & val2;
        case SOT_bitwise_or: return val1 | val2;
        default: assert (TablesErrCodes::PredicateComparisonNotDefined==0);
    }
    return false;  // should be unreachable
}

std::string buildKeyData(int data_type, uint64_t new_data) {
    std::string data_str = u64tostr(new_data);
    int len = data_str.length();
    int pos = 0;  // pos in u64string to get the minimum keychars for type
    switch (data_type) {
        case SDT_BOOL:
            pos = len-1;
            break;
        case SDT_CHAR:
        case SDT_UCHAR:
        case SDT_INT8:
        case SDT_UINT8:
            pos = len-3;
            break;
        case SDT_INT16:
        case SDT_UINT16:
            pos = len-5;
            break;
        case SDT_INT32:
        case SDT_UINT32:
            pos = len-10;
            break;
        case SDT_INT64:
        case SDT_UINT64:
            pos = 0;
            break;
    }
    return data_str.substr(pos, len);
}

std::string buildKeyPrefix(
        int idx_type,
        std::string schema_name,
        std::string table_name,
        std::vector<string> colnames) {

    boost::trim(schema_name);
    boost::trim(table_name);

    std::string idx_type_str;
    std::string key_cols_str;

    if (schema_name.empty())
        schema_name = SCHEMA_NAME_DEFAULT;

    if (table_name.empty())
        table_name = TABLE_NAME_DEFAULT;

    if (colnames.empty())
        key_cols_str = IDX_KEY_COLS_DEFAULT;

    switch (idx_type) {
    case SIT_IDX_FB:
        idx_type_str = SkyIdxTypeMap.at(SIT_IDX_FB);
        break;
    case SIT_IDX_RID:
        idx_type_str = SkyIdxTypeMap.at(SIT_IDX_RID);
        for (unsigned i = 0; i < colnames.size(); i++) {
            if (i > 0) key_cols_str += Tables::IDX_KEY_DELIM_INNER;
            key_cols_str += colnames[i];
        }
        break;
    case SIT_IDX_REC:
        idx_type_str =  SkyIdxTypeMap.at(SIT_IDX_REC);
        // stitch the colnames together
        for (unsigned i = 0; i < colnames.size(); i++) {
            if (i > 0) key_cols_str += Tables::IDX_KEY_DELIM_INNER;
            key_cols_str += colnames[i];
        }
        break;
    case SIT_IDX_TXT:
        idx_type_str =  SkyIdxTypeMap.at(SIT_IDX_TXT);
        break;
    default:
        idx_type_str = "IDX_UNK";
    }

    // TODO: this prefix should be encoded as a unique index number
    // to minimize key length/redundancy across keys
    return (
        idx_type_str + IDX_KEY_DELIM_OUTER +
        schema_name + IDX_KEY_DELIM_INNER +
        table_name + IDX_KEY_DELIM_OUTER +
        key_cols_str + IDX_KEY_DELIM_OUTER
    );
}

/*
 * Given a predicate vector, check if the opType provided is present therein.
   Used to compare idx ops, for special handling of leq case, etc.
*/
bool check_predicate_ops(predicate_vec index_preds, int opType)
{
    for (unsigned i = 0; i < index_preds.size(); i++) {
        if(index_preds[i]->opType() != opType)
            return false;
    }
    return true;
}

bool check_predicate_ops_all_include_equality(predicate_vec index_preds)
{
    for (unsigned i = 0; i < index_preds.size(); i++) {
        switch (index_preds[i]->opType()) {
            case SOT_eq:
            case SOT_leq:
            case SOT_geq:
                break;
            default:
                return false;
        }
    }
    return true;
}

bool check_predicate_ops_all_equality(predicate_vec index_preds)
{
    for (unsigned i = 0; i < index_preds.size(); i++) {
        switch (index_preds[i]->opType()) {
            case SOT_eq:
                break;
            default:
                return false;
        }
    }
    return true;
}

// used for index prefix matching during index range queries
bool compare_keys(std::string key1, std::string key2)
{

    // Format of keys is like IDX_REC:*-LINEITEM:LINENUMBER-ORDERKEY:00000000000000000001-00000000000000000006
    // First splitting both the string on the basis of ':' delimiter
    vector<std::string> elems1;
    boost::split(elems1, key1, boost::is_any_of(IDX_KEY_DELIM_OUTER),
                                                boost::token_compress_on);

    vector<std::string> elems2;
    boost::split(elems2, key2, boost::is_any_of(IDX_KEY_DELIM_OUTER),
                                                boost::token_compress_on);

    // 4th entry in vector represents the value vector i.e after prefix
    vector<std::string> value1;
    vector<std::string> value2;

    // Now splitting value field on the basis of '-' delimiter
    boost::split(value1,
                 elems1[IDX_FIELD_Value],
                 boost::is_any_of(IDX_KEY_DELIM_INNER),
                 boost::token_compress_on);
    boost::split(value2,
                 elems2[IDX_FIELD_Value],
                 boost::is_any_of(IDX_KEY_DELIM_INNER),
                 boost::token_compress_on);

   // Compare first token of both field value
    if (!value1.empty() and !value2.empty()) {
        if(value1[0] == value2[0])
            return true;
    }
    return false;
}

void extract_typedpred_val(Tables::PredicateBase* pb, int64_t& val) {

    switch(pb->colType()) {

        case SDT_INT8: {
            TypedPredicate<int8_t>* p = \
                dynamic_cast<TypedPredicate<int8_t>*>(pb);
            val = static_cast<int64_t>(p->Val());
            break;
        }
        case SDT_INT16: {
            TypedPredicate<int16_t>* p = \
                dynamic_cast<TypedPredicate<int16_t>*>(pb);
            val = static_cast<int64_t>(p->Val());
            break;
        }
        case SDT_INT32: {
            TypedPredicate<int32_t>* p = \
                dynamic_cast<TypedPredicate<int32_t>*>(pb);
            val = static_cast<uint64_t>(p->Val());
            break;
        }
        case SDT_INT64: {
            TypedPredicate<int64_t>* p = \
                dynamic_cast<TypedPredicate<int64_t>*>(pb);
            val = static_cast<int64_t>(p->Val());
            break;
        }
        default:
            assert (BuildSkyIndexUnsupportedColType==0);
    }
}

void extract_typedpred_val(Tables::PredicateBase* pb, uint64_t& val) {

    switch(pb->colType()) {

        case SDT_UINT8: {
            TypedPredicate<uint8_t>* p = \
                dynamic_cast<TypedPredicate<uint8_t>*>(pb);
            val = static_cast<uint64_t>(p->Val());
            break;
        }
        case SDT_UINT16: {
            TypedPredicate<uint16_t>* p = \
                dynamic_cast<TypedPredicate<uint16_t>*>(pb);
            val = static_cast<uint64_t>(p->Val());
            break;
        }
        case SDT_UINT32: {
            TypedPredicate<uint32_t>* p = \
                dynamic_cast<TypedPredicate<uint32_t>*>(pb);
            val = static_cast<uint64_t>(p->Val());
            break;
        }
        case SDT_UINT64: {
            TypedPredicate<uint64_t>* p = \
                dynamic_cast<TypedPredicate<uint64_t>*>(pb);
            val = static_cast<uint64_t>(p->Val());
            break;
        }
        default:
            assert (BuildSkyIndexUnsupportedColType==0);
    }
}

/* @todo: This is a temporary function to demonstrate buffer is read from the file.
 * In reality, Ceph will return a bufferlist containing a buffer.
 */

int read_from_file(const char *filename, std::shared_ptr<arrow::Buffer> *buffer)
{
  // Open file
  std::ifstream infile(filename);

  // Get length of file
  infile.seekg(0, infile.end);
  size_t length = infile.tellg();
  infile.seekg(0, infile.beg);

  std::unique_ptr<char[]> cdata{new char [length + 1]};

  // Read file
  infile.read(cdata.get(), length);
  std::string data(cdata.get(), length);
  arrow::Buffer::FromString(data, buffer);
  return 0;
}


/* @todo: This is a temporary function to demonstrate buffer is written on to a file.
 * In reality, the buffer is given to Ceph which takes care of writing.
 */

int write_to_file(const char *filename, arrow::Buffer* buffer)
{
    std::ofstream ofile("/tmp/skyhook.arrow");
    std::string str(buffer->ToString());
    ofile.write(buffer->ToString().c_str(), buffer->size());
    return 0;
}

/*
 * Function: extract_arrow_from_buffer
 * Description: Extract arrow table from a buffer.
 *  a. Where to read - In this case we are using buffer, but it can be streams or
 *                     files as well.
 *  b. InputStream - We connect a buffer (or file) to the stream and reader will read
 *                   data from this stream. We are using arrow::InputStream as an
 *                   input stream.
 *  c. Reader - Reads the data from InputStream. We are using arrow::RecordBatchReader
 *              which will read the data from input stream.
 * @param[out] table  : Arrow table to be converted
 * @param[in] buffer  : Input buffer
 * Return Value: error code
 */
int extract_arrow_from_buffer(std::shared_ptr<arrow::Table>* table, const std::shared_ptr<arrow::Buffer> &buffer)
{
    // Initialization related to reading from a buffer
    const std::shared_ptr<arrow::io::InputStream> buf_reader = std::make_shared<arrow::io::BufferReader>(buffer);
    std::shared_ptr<arrow::ipc::RecordBatchReader> reader;
    arrow::ipc::RecordBatchStreamReader::Open(buf_reader, &reader);

    // Initilaization related to read to apache arrow
    std::vector<std::shared_ptr<arrow::RecordBatch>> batch_vec;
    while (true){
        std::shared_ptr<arrow::RecordBatch> chunk;
        reader->ReadNext(&chunk);
        if (chunk == nullptr) break;
        batch_vec.push_back(chunk);
    }

    arrow::Table::FromRecordBatches(batch_vec, table);
    return 0;
}

/*
 * Function: convert_arrow_to_buffer
 * Description: Convert arrow table into record batches which are dumped on to a
 *              output buffer. For converting arrow table three things are essential.
 *  a. Where to write - In this case we are using buffer, but it can be streams or
 *                      files as well
 *  b. OutputStream - We connect a buffer (or file) to the stream and writer writes
 *                    data from this stream. We are using arrow::BufferOutputStream
 *                    as an output stream.
 *  c. Writer - Does writing data to OutputStream. We are using arrow::RecordBatchStreamWriter
 *              which will write the data to output stream.
 * @param[in] table   : Arrow table to be converted
 * @param[out] buffer : Output buffer
 * Return Value: error code
 */
int convert_arrow_to_buffer(const std::shared_ptr<arrow::Table> &table, std::shared_ptr<arrow::Buffer>* buffer)
{
    // Initilization related to writing to the the file
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer;
    std::shared_ptr<arrow::io::BufferOutputStream> out;
    arrow::io::BufferOutputStream::Create(STREAM_CAPACITY, arrow::default_memory_pool(), &out);
    arrow::io::OutputStream *raw_out = out.get();
    arrow::Table *raw_table = table.get();
    arrow::ipc::RecordBatchStreamWriter::Open(raw_out, raw_table->schema(), &writer);

    // Initilization related to reading from arrow
    writer->WriteTable(*(table.get()));
    writer->Close();
    out->Finish(buffer);
    return 0;
}

/*
 * Function: compress_arrow_tables
 * Description: Compress the given arrow tables into single arrow table. Before
 * compression check if schema for all that tables are same.
 * @param[in] table_vec : Vector of tables to be compressed
 * @param[out] table    : Output arrow table
 * Return Value: error code
 */
int compress_arrow_tables(std::vector<std::shared_ptr<arrow::Table>> &table_vec,
                          std::shared_ptr<arrow::Table> *table)
{
    auto table_ptr = *table_vec.begin();
    auto original_schema = (table_ptr)->schema();

    // Check if schema for all tables are same, otherwise return error
    for (auto it = table_vec.begin(); it != table_vec.end(); it++) {
        auto table_schema = (*it)->schema();
        if (!original_schema->Equals(*table_schema.get()))
            return TablesErrCodes::ArrowStatusErr;
    }

    // TODO: Change schema metadata for the created table
    return 0;
}


/*
 * Function: split_arrow_table
 * Description: Split the given arrow table into number of arrow tables. Based on
 * input parameter (max_rows) spliting is done.
 * @param[in] table      : Table to be split.
 * @param[out] max_rows  : Maximum number of rows a table can have.
 * @param[out] table_vec : Vector of tables created after split.
 * Return Value: error code
 */
int split_arrow_table(std::shared_ptr<arrow::Table> &table, int max_rows,
                      std::vector<std::shared_ptr<arrow::Table>>* table_vec)
{
    auto orig_schema = table->schema();
    auto orig_metadata = orig_schema->metadata();
    int orig_num_cols = table->num_columns();
    int remaining_rows = std::stoi(orig_metadata->value(METADATA_NUM_ROWS));
    int offset = 0;

    while ((remaining_rows / max_rows) >= 1) {

        // Extract skyhook metadata from original table.
        std::shared_ptr<arrow::KeyValueMetadata> metadata (new arrow::KeyValueMetadata);
        metadata->Append(ToString(METADATA_SKYHOOK_VERSION),
                         orig_metadata->value(METADATA_SKYHOOK_VERSION));
        metadata->Append(ToString(METADATA_DATA_SCHEMA_VERSION),
                         orig_metadata->value(METADATA_DATA_SCHEMA_VERSION));
        metadata->Append(ToString(METADATA_DATA_STRUCTURE_VERSION),
                         orig_metadata->value(METADATA_DATA_STRUCTURE_VERSION));
        metadata->Append(ToString(METADATA_DATA_FORMAT_TYPE),
                         orig_metadata->value(METADATA_DATA_FORMAT_TYPE));
        metadata->Append(ToString(METADATA_DATA_SCHEMA),
                         orig_metadata->value(METADATA_DATA_SCHEMA));
        metadata->Append(ToString(METADATA_DB_SCHEMA),
                         orig_metadata->value(METADATA_DB_SCHEMA));
        metadata->Append(ToString(METADATA_TABLE_NAME),
                         orig_metadata->value(METADATA_TABLE_NAME));

        if (remaining_rows <= max_rows)
            metadata->Append(ToString(METADATA_NUM_ROWS),
                             std::to_string(remaining_rows));
        else
            metadata->Append(ToString(METADATA_NUM_ROWS),
                             std::to_string(max_rows));

        // Generate the schema for new table using original table schema
        auto schema = std::make_shared<arrow::Schema>(orig_schema->fields(), metadata);

        // Split and create the columns from original table
        std::vector<std::shared_ptr<arrow::Column>> column_list;
        for (int i = 0; i < orig_num_cols; i++) {
            std::shared_ptr<arrow::Column> column;
            if (remaining_rows <= max_rows)
                column = table->column(i)->Slice(offset);
            else
                column = table->column(i)->Slice(offset, max_rows);
            column_list.emplace_back(column);
        }
        offset += max_rows;

        // Finally, create the arrow table based on schema and column vector
        std::shared_ptr<arrow::Table> table = arrow::Table::Make(schema, column_list);
        table_vec->push_back(table);
        remaining_rows -= max_rows;
    }
    return 0;
}

int print_arrowbuf_colwise(std::shared_ptr<arrow::Table>& table)
{
    std::vector<std::shared_ptr<arrow::Array>> array_list;

    // From Table get the schema and from schema get the skyhook schema
    // which is stored as a metadata
    auto schema = table->schema();
    auto metadata = schema->metadata();
    schema_vec sc = schemaFromString(metadata->value(METADATA_DATA_SCHEMA));

    // Iterate through each column in print the data inside it
    for (auto it = sc.begin(); it != sc.end(); ++it) {
        col_info col = *it;
        std::cout << table->column(col.idx)->name();
        std::cout << CSV_DELIM;
        std::vector<std::shared_ptr<arrow::Array>> array_list = table->column(col.idx)->data()->chunks();

        switch(col.type) {
            case SDT_BOOL: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::BooleanArray>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_INT8: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::Int8Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_INT16: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::Int16Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
            }
            case SDT_INT32: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::Int32Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_INT64: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::Int64Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_UINT8: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::UInt8Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_UINT16: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::UInt16Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_UINT32: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::UInt32Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_UINT64: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::UInt64Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_CHAR: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::Int8Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_UCHAR: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::UInt8Array>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_FLOAT: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::FloatArray>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_DOUBLE: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::to_string(std::static_pointer_cast<arrow::DoubleArray>(array)->Value(j));
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            case SDT_DATE:
            case SDT_STRING: {
                for (auto it = array_list.begin(); it != array_list.end(); ++it) {
                    auto array = *it;
                    for (int j = 0; j < array->length(); j++) {
                        std::cout << std::static_pointer_cast<arrow::StringArray>(array)->GetString(j);
                        std::cout << CSV_DELIM;
                    }
                }
                break;
            }
            default:
                return TablesErrCodes::UnsupportedSkyDataType;
        }
        std::cout << std::endl;  // newline to start next row.
    }
    return 0;
}

void printArrowHeader(std::shared_ptr<const arrow::KeyValueMetadata> &metadata)
{
    std::cout << "\n\n\n[SKYHOOKDB ROOT HEADER (arrow)]" << std::endl;
    std::cout << metadata->key(METADATA_SKYHOOK_VERSION).c_str() << ": "
              << metadata->value(METADATA_SKYHOOK_VERSION).c_str() << std::endl;
    std::cout << metadata->key(METADATA_DATA_SCHEMA_VERSION).c_str() << ": "
              << metadata->value(METADATA_DATA_SCHEMA_VERSION).c_str() << std::endl;
    std::cout << metadata->key(METADATA_DATA_STRUCTURE_VERSION).c_str() << ": "
              << metadata->value(METADATA_DATA_STRUCTURE_VERSION).c_str() << std::endl;
    std::cout << metadata->key(METADATA_DATA_FORMAT_TYPE).c_str() << ": "
              << metadata->value(METADATA_DATA_FORMAT_TYPE).c_str() << std::endl;
    std::cout << metadata->key(METADATA_NUM_ROWS).c_str() << ": "
              << metadata->value(METADATA_NUM_ROWS).c_str() << std::endl;
}

long long int printArrowbufRowAsCsv(const char* dataptr,
                                    const size_t datasz,
                                    bool print_header,
                                    bool print_verbose,
                                    long long int max_to_print)
{
    // Each column in arrow is represented using Chunked Array. A chunked array is
    // a vector of chunks i.e. arrays which holds actual data.

    // Declare vector for columns (i.e. chunked_arrays)
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> chunked_array_vec;
    std::shared_ptr<arrow::Table> table;
    std::shared_ptr<arrow::Buffer> buffer;

    std::string str_data(dataptr, datasz);
    arrow::Buffer::FromString(str_data, &buffer);

    extract_arrow_from_buffer(&table, buffer);
    // From Table get the schema and from schema get the skyhook schema
    // which is stored as a metadata
    auto schema = table->schema();
    auto metadata = schema->metadata();
    schema_vec sc = schemaFromString(metadata->value(METADATA_DATA_SCHEMA));
    int num_rows = std::stoi(metadata->value(METADATA_NUM_ROWS));
    int num_cols = 0;

    if (print_verbose)
        printArrowHeader(metadata);

    // Get the names of each column and get the vector of chunks
    for (auto it = sc.begin(); it != sc.end(); ++it) {
        col_info col = *it;
        if (print_header) {
            std::cout << table->column(col.idx)->name();
            if (it->is_key) std::cout << "(key)";
            if (!it->nullable) std::cout << "(NOT NULL)";
            std::cout << CSV_DELIM;
        }
        chunked_array_vec.emplace_back(table->column(col.idx)->data()->chunks());
    }

    if (print_verbose) {
        num_cols = std::distance(sc.begin(), sc.end());

        if (print_header) {
            std::cout << table->column(ARROW_RID_INDEX(num_cols))->name()
                      << CSV_DELIM;
            std::cout << table->column(ARROW_DELVEC_INDEX(num_cols))->name()
                      << CSV_DELIM;
        }

        // Add RID and delete vector column
        chunked_array_vec.emplace_back(table->column(ARROW_RID_INDEX(num_cols))->data()->chunks());
        chunked_array_vec.emplace_back(table->column(ARROW_DELVEC_INDEX(num_cols))->data()->chunks());
    }

    if (print_header)
        std::cout << std::endl;

    // As number of elements in all the columns are equal use chunked_array 0 to get number of
    // elements.
    int array_index = 0;
    auto array_it = chunked_array_vec[0];

    // Get the number of elements in the first array.
    auto array = array_it[array_index];
    int array_num_elements = array->length();
    int array_element_it = 0;
    for (int i = 0; i < num_rows; i++, array_element_it++) {

        // Check if we have exhausted the current array. If yes,
        // go to next array inside the chunked array to get the number of
        // element in the next array.
        if (array_element_it == array_num_elements) {
            array_index++;
            array = array_it[array_index];
            array_num_elements = array->length();
            array_element_it = 0;
        }

        // For this row get the data from each columns
        for (auto it = sc.begin(); it != sc.end(); ++it) {
            col_info col = *it;
            auto print_array_it = chunked_array_vec[std::distance(sc.begin(), it)];
            auto print_array = print_array_it[array_index];

            if (print_array->IsNull(array_element_it)) {
                std::cout << "NULL" << CSV_DELIM;
                continue;
            }

            switch(col.type) {
                case SDT_BOOL: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::BooleanArray>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_INT8: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::Int8Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_INT16: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::Int16Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_INT32: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::Int32Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_INT64: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::Int64Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_UINT8: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::UInt8Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_UINT16: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::UInt16Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_UINT32: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::UInt32Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_UINT64: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::UInt64Array>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_CHAR: {
                    std::cout << (char)std::static_pointer_cast<arrow::Int8Array>(print_array)->Value(array_element_it);
                    break;
                }
                case SDT_UCHAR: {
                    std::cout << (char)std::static_pointer_cast<arrow::UInt8Array>(print_array)->Value(array_element_it);
                    break;
                }
                case SDT_FLOAT: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::FloatArray>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_DOUBLE: {
                    std::cout << std::to_string(std::static_pointer_cast<arrow::DoubleArray>(print_array)->Value(array_element_it));
                    break;
                }
                case SDT_DATE:
                case SDT_STRING: {
                    std::cout << std::static_pointer_cast<arrow::StringArray>(print_array)->GetString(array_element_it);
                    break;
                }
                default: {
                    return TablesErrCodes::UnsupportedSkyDataType;
                }
            }
            std::cout << CSV_DELIM;
        }
        if (print_verbose) {
            // Print RID
            auto print_array_it = chunked_array_vec[ARROW_RID_INDEX(num_cols)];
            auto print_array = print_array_it[array_index];
            std::cout << std::to_string(std::static_pointer_cast<arrow::Int64Array>(print_array)->Value(array_element_it)) << CSV_DELIM;

            // Print Deleted Vector
            print_array_it = chunked_array_vec[ARROW_DELVEC_INDEX(num_cols)];
            print_array = print_array_it[array_index];
            std::cout << std::to_string(std::static_pointer_cast<arrow::UInt8Array>(print_array)->Value(array_element_it)) << CSV_DELIM;
        }
        std::cout << std::endl;  // newline to start next row.
    }
    return 0;
}

/*
 * Function: transform_fb_to_arrow
 * Description: Build arrow schema vector using skyhook schema information. Get the
 *              details of columns from skyhook schema and using array builders for
 *              each datatype add the data to array vectors. Finally, create arrow
 *              table using array vectors and schema vector.
 * @param[in] fb      : Flatbuffer to be converted
 * @param[in] size    : Size of the flatbuffer
 * @param[out] errmsg : errmsg buffer
 * @param[out] buffer : arrow table
 * Return Value: error code
 */
int transform_fb_to_arrow(const char* fb,
                          const size_t fb_size,
                          std::string& errmsg,
                          std::shared_ptr<arrow::Table>* table)
{
    int errcode = 0;
    sky_root root = getSkyRoot(fb, fb_size);
    schema_vec sc = schemaFromString(root.data_schema);
    delete_vector del_vec = root.delete_vec;
    uint32_t nrows = root.nrows;

    // Initialization related to Apache Arrow
    auto pool = arrow::default_memory_pool();
    std::vector<arrow::ArrayBuilder *> builder_list;
    std::vector<std::shared_ptr<arrow::Array>> array_list;
    std::vector<std::shared_ptr<arrow::Field>> schema_vector;
    std::shared_ptr<arrow::KeyValueMetadata> metadata (new arrow::KeyValueMetadata);

    // Add skyhook metadata to arrow metadata.
    metadata->Append(ToString(METADATA_SKYHOOK_VERSION),
                     std::to_string(root.skyhook_version));
    metadata->Append(ToString(METADATA_DATA_SCHEMA_VERSION),
                     std::to_string(root.data_schema_version));
    metadata->Append(ToString(METADATA_DATA_STRUCTURE_VERSION),
                     std::to_string(root.data_structure_version));
    metadata->Append(ToString(METADATA_DATA_FORMAT_TYPE),
                     std::to_string(root.data_format_type));
    metadata->Append(ToString(METADATA_DATA_SCHEMA), root.data_schema);
    metadata->Append(ToString(METADATA_DB_SCHEMA), root.db_schema_name);
    metadata->Append(ToString(METADATA_TABLE_NAME), root.table_name);
    metadata->Append(ToString(METADATA_NUM_ROWS), std::to_string(root.nrows));

    // Iterate through schema vector to get the details of columns i.e name and type.
    for (auto it = sc.begin(); it != sc.end() && !errcode; ++it) {
        col_info col = *it;

        // Create the array builders for respective datatypes. Use these array
        // builders to store data to array vectors. These array vectors holds the
        // actual column values. Also, add the details of column

        switch(col.type) {

            case SDT_BOOL: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::BooleanBuilder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::boolean()));
                break;
            }
            case SDT_INT8: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int8Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::int8()));
                break;
            }
            case SDT_INT16: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int16Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::int16()));
                break;
            }
            case SDT_INT32: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int32Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::int32()));
                break;
            }
            case SDT_INT64: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int64Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::int64()));
                break;
            }
            case SDT_UINT8: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::UInt8Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::uint8()));
                break;
            }
            case SDT_UINT16: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::UInt16Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::uint16()));
                break;
            }
            case SDT_UINT32: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::UInt32Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::uint32()));
                break;
            }
            case SDT_UINT64: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::UInt64Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::uint64()));
                break;
            }
            case SDT_FLOAT: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::FloatBuilder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::float32()));
                break;
            }
            case SDT_DOUBLE: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::DoubleBuilder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::float64()));
                break;
            }
            case SDT_CHAR: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int8Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::int8()));
                break;
            }
            case SDT_UCHAR: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::UInt8Builder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::uint8()));
                break;
            }
            case SDT_DATE:
            case SDT_STRING: {
                auto ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::StringBuilder(pool));
                builder_list.emplace_back(ptr.get());
                ptr.release();
                schema_vector.push_back(arrow::field(col.name, arrow::utf8()));
                break;
            }
            default: {
                errcode = TablesErrCodes::UnsupportedSkyDataType;
                errmsg.append("ERROR transform_row_to_col(): table=" +
                              root.table_name + " col.type=" +
                              std::to_string(col.type) +
                              " UnsupportedSkyDataType.");
                return errcode;
            }
        }
    }

    // Add RID column
    auto rid_ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::Int64Builder(pool));
    builder_list.emplace_back(rid_ptr.get());
    rid_ptr.release();
    schema_vector.push_back(arrow::field("RID", arrow::int64()));

    // Add deleted vector column
    auto dv_ptr = std::unique_ptr<arrow::ArrayBuilder>(new arrow::BooleanBuilder(pool));
    builder_list.emplace_back(dv_ptr.get());
    dv_ptr.release();
    schema_vector.push_back(arrow::field("DELETED_VECTOR", arrow::boolean()));

    // Iterate through rows and store data in each row in respective columns.
    for (uint32_t i = 0; i < nrows; i++) {

        // Get a skyhook record struct
        sky_rec rec = getSkyRec(static_cast<row_offs>(root.data_vec)->Get(i));

        // Get the row as a vector.
        auto row = rec.data.AsVector();

        // For the current row, go from 0 to num_cols and append the data into array
        // builders.
        for (auto it = sc.begin(); it != sc.end() && !errcode; ++it) {
            auto builder = builder_list[std::distance(sc.begin(), it)];
            col_info col = *it;

            if (col.nullable) {  // check nullbit
                bool is_null = false;
                int pos = col.idx / (8*sizeof(rec.nullbits.at(0)));
                int col_bitmask = 1 << col.idx;
                if ((col_bitmask & rec.nullbits.at(pos)) == 1)  {
                    is_null = true;
                }
                if (is_null) {
                    builder->AppendNull();
                    continue;
                }
            }

            // Append data to the respective data type builders
            switch(col.type) {

                case SDT_BOOL:
                    static_cast<arrow::BooleanBuilder *>(builder)->Append(row[col.idx].AsBool());
                    break;
                case SDT_INT8:
                    static_cast<arrow::Int8Builder *>(builder)->Append(row[col.idx].AsInt8());
                    break;
                case SDT_INT16:
                    static_cast<arrow::Int16Builder *>(builder)->Append(row[col.idx].AsInt16());
                    break;
                case SDT_INT32:
                    static_cast<arrow::Int32Builder *>(builder)->Append(row[col.idx].AsInt32());
                    break;
                case SDT_INT64:
                    static_cast<arrow::Int64Builder *>(builder)->Append(row[col.idx].AsInt64());
                    break;
                case SDT_UINT8:
                    static_cast<arrow::UInt8Builder *>(builder)->Append(row[col.idx].AsUInt8());
                    break;
                case SDT_UINT16:
                    static_cast<arrow::UInt16Builder *>(builder)->Append(row[col.idx].AsUInt16());
                    break;
                case SDT_UINT32:
                    static_cast<arrow::UInt32Builder *>(builder)->Append(row[col.idx].AsUInt32());
                    break;
                case SDT_UINT64:
                    static_cast<arrow::UInt64Builder *>(builder)->Append(row[col.idx].AsUInt64());
                    break;
                case SDT_FLOAT:
                    static_cast<arrow::FloatBuilder *>(builder)->Append(row[col.idx].AsFloat());
                    break;
                case SDT_DOUBLE:
                    static_cast<arrow::DoubleBuilder *>(builder)->Append(row[col.idx].AsDouble());
                    break;
                case SDT_CHAR:
                    static_cast<arrow::Int8Builder *>(builder)->Append(row[col.idx].AsInt8());
                    break;
                case SDT_UCHAR:
                    static_cast<arrow::UInt8Builder *>(builder)->Append(row[col.idx].AsUInt8());
                    break;
                case SDT_DATE:
                case SDT_STRING:
                    static_cast<arrow::StringBuilder *>(builder)->Append(row[col.idx].AsString().str());
                    break;
                default: {
                    errcode = TablesErrCodes::UnsupportedSkyDataType;
                    errmsg.append("ERROR transform_row_to_col(): table=" +
                                  root.table_name + " col.type=" +
                                  std::to_string(col.type) +
                                  " UnsupportedSkyDataType.");
                }
            }
        }

        // Add entries for RID and Deleted vector
        int num_cols = std::distance(sc.begin(), sc.end());
        static_cast<arrow::Int64Builder *>(builder_list[ARROW_RID_INDEX(num_cols)])->Append(rec.RID);
        static_cast<arrow::UInt8Builder *>(builder_list[ARROW_DELVEC_INDEX(num_cols)])->Append(del_vec[i]);

    }

    // Finalize the arrays holding the data
    for (auto it = builder_list.begin(); it != builder_list.end(); ++it) {
        auto builder = *it;
        std::shared_ptr<arrow::Array> array;
        builder->Finish(&array);
        array_list.push_back(array);
        delete builder;
    }

    // Generate schema from schema vector and add the metadata
    auto schema = std::make_shared<arrow::Schema>(schema_vector, metadata);

    // Finally, create a arrow table from schema and array vector
    *table = arrow::Table::Make(schema, array_list);

    return errcode;
}

int transform_arrow_to_fb(const char* data,
                          const size_t data_size,
                          std::string& errmsg,
                          flatbuffers::FlatBufferBuilder& flatbldr)
{
    int ret;

    // Placeholder function
    std::shared_ptr<arrow::Table> table;
    std::shared_ptr<arrow::Buffer> buffer;

    std::string str_data(data, data_size);
    arrow::Buffer::FromString(str_data, &buffer);

    extract_arrow_from_buffer(&table, buffer);

    ret = print_arrowbuf_colwise(table);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

//TODO: remove
/*
This is a test code for checking decode arrow table works fine or not
int test_bls(bufferlist *wrapped_bls)
{
    // Create bl1
    bufferlist bl1;
    std::shared_ptr<arrow::Table> table1;
    std::shared_ptr<arrow::Buffer> buffer1;
    read_from_file("/tmp/skyhook_1.arrow", &buffer1);
    bl1.append(buffer1->ToString().c_str(), buffer1->size());
    ::encode(bl1, *wrapped_bls);

    // Create bl2
    bufferlist bl2;
    std::shared_ptr<arrow::Table> table2;
    std::shared_ptr<arrow::Buffer> buffer2;
    read_from_file("/tmp/skyhook_2.arrow", &buffer2);
    bl2.append(buffer2->ToString().c_str(), buffer2->size());
    ::encode(bl2, *wrapped_bls);
    return 0;
}*/

} // end namespace Tables