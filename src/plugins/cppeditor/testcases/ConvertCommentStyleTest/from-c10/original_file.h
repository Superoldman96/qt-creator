/** Expand envi@{start}ronment variables in a string.
 *
 * Environment variables are accepted in the @{end}following forms:
 * $SOMEVAR, ${SOMEVAR} on Unix and %SOMEVAR% on Windows.
 * No escapes and quoting are supported.
 * If a variable is not found, it is not substituted.
 */
