project(oig)

set(EOSIO_WASM_OLD_BEHAVIOR "Off")
find_package(eosio.cdt)

add_contract( oig oig oig.cpp )
target_include_directories( oig PUBLIC ${CMAKE_SOURCE_DIR}/../include )
target_ricardian_directory( oig ${CMAKE_SOURCE_DIR}/../ricardian )