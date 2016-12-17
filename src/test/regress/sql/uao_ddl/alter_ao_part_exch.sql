--- Define a multi-leveled partitioned table with partitions of
--- different types (ao, aoco, heap). Then, exchange these partitions
--- with other tables.
BEGIN;
Create table ao_part
 (
 col1 bigint, col2 date, col3 text, col4 int) with(appendonly=true)
 distributed randomly partition by range(col2)
 subpartition by list (col3)
 subpartition template (
   default subpartition subothers,
   subpartition sub1 values ('ao_row'),
   subpartition sub2 values ('heap') with (appendonly=false),
   subpartition sub3 values ('ao_col') with (appendonly=true, orientation=column))
 (default partition others, start(date '2008-01-01') end(date '2008-04-30') every(interval '1 month'));

insert into ao_part(col1, col2, col3) values
 (1, '2008-01-02', 'ao_row'), (1, '2008-01-02', 'heap'), (1, '2008-01-02', 'ao_col'),
 (1, '2008-02-03', 'ao_row'), (1, '2008-02-03', 'heap'), (1, '2008-02-03', 'ao_col'),
 (1, '2008-03-04', 'ao_row'), (1, '2008-03-04', 'heap'), (1, '2008-03-04', 'ao_col'),
 (1, '2008-04-15', 'ao_row'), (1, '2008-04-15', 'heap'), (1, '2008-04-05', 'ao_col'),
 (1, '2008-05-06', 'ao_row');


select count(*) FROM pg_appendonly WHERE visimapidxid is not NULL AND
visimapidxid is not NULL AND relid in (SELECT oid FROM pg_class WHERE
relname like 'ao_part%');

select relkind, relstorage from pg_class where relname like
'ao_part_1_prt_5_2_prt_subothers%';

--- Exchange partitions
create table exh_ao_ao (like ao_part) with (appendonly=true) distributed randomly;
insert into exh_ao_ao values (1, '2008-02-20', 'ao_row'), (2, '2008-02-21', 'ao_row');
Alter table ao_part alter partition for (rank(2)) exchange partition for ('ao_row') with table exh_ao_ao;
update ao_part set col4 = col1 where col2 in ('2008-02-20', '2008-02-21');

create table exh_ao_co (like ao_part) with (appendonly=true, orientation=column) distributed randomly;
insert into exh_ao_co values (1, '2008-03-20', 'ao_row'),  (2, '2008-03-21', 'ao_row');
Alter table ao_part alter partition for (rank(3)) exchange partition for ('ao_row') with table exh_ao_co;
delete from ao_part where col2 = '2008-03-20';

create table exh_ao_heap (like ao_part) distributed randomly;
insert into exh_ao_heap values (1, '2008-04-20', 'ao_row'),  (2, '2008-04-21', 'ao_row');
Alter table ao_part alter partition for (rank(4)) exchange partition for ('ao_row') with table exh_ao_heap;
update ao_part set col4 = -col1 where col2 in ('2008-04-20', '2008-04-21');
select count(*) from ao_part where col4 < 0;

create table exh_heap_ao (like ao_part) with (appendonly=true) distributed randomly;
insert into exh_heap_ao values (1, '2008-01-20', 'heap'), (2, '2008-01-21', 'heap');
Alter table ao_part alter partition for (rank(1)) exchange partition for ('heap') with table exh_heap_ao;
update ao_part set col4 = col1 where col2 in ('2008-01-20', '2008-01-21');
select * from ao_part where col2 in ('2008-01-20', '2008-01-21') and col3 = 'heap';

create table exh_co_ao (like ao_part) with (appendonly=true) distributed randomly;
insert into exh_co_ao values (1, '2008-01-20', 'ao_col'), (2, '2008-01-21', 'ao_col');
Alter table ao_part alter partition for (rank(1)) exchange partition for ('ao_col') with table exh_co_ao;
update ao_part set col4 = -col1 where col4 is null;
select count(*) from ao_part where col4 is not null;

select relkind, relstorage from pg_class where relname like 'ao_part_1_prt_5_2_prt_subothers%';

select count(*) from exh_ao_ao;
select count(*) from exh_ao_co;
select count(*) from exh_ao_heap;
select count(*) from exh_heap_ao;
select count(*) from exh_co_ao;

--- This touches 5 rows, so invisible row count increases by 5
Update ao_part set col4=2 where col2='2008-01-20' OR col2='2008-02-20'
 OR col2='2008-03-20' OR col2='2008-04-20';

select count(*) as visible_tuples from ao_part;
set gp_select_invisible = true;
select count(*) as all_tuples from ao_part;
set gp_select_invisible = false;

--- This touches 5 rows
Delete from ao_part where col2='2008-01-21' OR col2='2008-02-21'
 OR col2='2008-03-21' OR col2='2008-04-21';

select count(*) as visible_tuples from ao_part;
set gp_select_invisible = true;
select count(*) as all_tuples from ao_part;
set gp_select_invisible = false;
COMMIT;
