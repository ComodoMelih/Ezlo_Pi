/* Host stub for nvs_flash.h -- deliberately empty of declarations.
 *
 * ezlopi_core_nvs.h includes it, and the scenes then-methods translation unit
 * includes that. Nothing in the subject names a type or constant from it: the
 * one NVS entry point the subject calls, EZPI_core_nvs_read_scenes_expressions,
 * is declared by ezlopi_core_nvs.h itself and stubbed in stub_httpurl.c.
 *
 * Measured, not assumed: hostable.py drove the compiler over the subject with
 * every absent header replaced by an empty file, and the only diagnostics left
 * were six unknown types, none of them from here. If that changes, this file
 * is where the missing piece goes -- and the compiler will say so rather than
 * the test quietly covering less.
 *
 * See the test_httpurl rule in ../Makefile for why seven stubs exist at all.
 */
#ifndef _HOST_STUB_NVS_FLASH_H_
#define _HOST_STUB_NVS_FLASH_H_
#endif /* _HOST_STUB_NVS_FLASH_H_ */
