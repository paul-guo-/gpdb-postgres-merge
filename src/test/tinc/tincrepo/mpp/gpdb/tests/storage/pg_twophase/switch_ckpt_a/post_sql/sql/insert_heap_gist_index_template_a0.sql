\d pg2_heap_table_gist_index_a0

set enable_seqscan=off;
select property from pg2_heap_table_gist_index_a0 where property='( (0,0), (1,1) )';

DROP TABLE pg2_heap_table_gist_index_a0;
