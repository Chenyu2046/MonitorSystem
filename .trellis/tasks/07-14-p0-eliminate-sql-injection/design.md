# Design

The existing MySQL client library already provides `mysql_real_escape_string`,
which applies the active connection charset. Use a small helper at each MySQL
owner rather than a broad prepared-statement rewrite across five existing write
layouts and nine read layouts. This removes executable string-literal escapes
without changing the schema or query shapes.

Numeric values remain typed C++ values; later P1 validation will bound values,
pagination, and time ranges separately.
