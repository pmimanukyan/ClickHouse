CREATE TEMPORARY TABLE table (x UInt8);
INSERT INTO `table` FORMAT Values (1);
INSERT INTO TABLE `table` FORMAT Values (2);
INSERT INTO TABLE table FORMAT Values (3);
SELECT * FROM table ORDER BY x;
DROP TABLE table;

CREATE TEMPORARY TABLE FORMAT (x UInt8);
INSERT INTO table FORMAT Values (1);
SELECT * FROM FORMAT FORMAT Values;
