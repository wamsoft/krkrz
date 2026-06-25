# MergeVcpkgManifest.cmake
#
# 複数の vcpkg.json (BASE と SUBDIRS) を 1 つにマージして OUTPUT に書き出す。
# 主要な使い方は project() 呼び出し前に実行し、VCPKG_MANIFEST_DIR を OUTPUT の
# ディレクトリに向けることで、vcpkg manifest install にマージ済みマニフェストを
# 読ませるユースケース。
#
# - dependencies は name 単位で重複排除 (BASE 側を優先、SUB 側は新規のみ追加)
# - builtin-baseline / vcpkg-configuration / overrides は BASE のものを採用、
#   SUB 側は無視する (同じ baseline で揃えるのは PR レビュー側の責任)
# - SUBDIRS で vcpkg.json が存在しないパスは静かにスキップ
# - 拾った各 vcpkg.json を CMAKE_CONFIGURE_DEPENDS に登録するので、サブの
#   依存追加だけで cmake 再生成がトリガーされる
#
# EXCLUDE で SUB 側エントリを除外できる。指定形式:
#   "<dep>"             … サブのどこから来ても除外 (グローバル)
#   "<basename>:<dep>"  … 当該 SUBDIR (basename はパス末尾) からのみ除外
# 例: EXCLUDE "movie-player:glew" "movie-player:glfw3" は movie-player の
#   vcpkg.json から glew / glfw3 だけ拾わない。BASE 側のエントリは EXCLUDE の
#   影響を受けない (BASE は手で編集する)。

function(krkrz_merge_vcpkg_manifest)
    set(options "")
    set(oneValueArgs BASE OUTPUT)
    set(multiValueArgs SUBDIRS EXCLUDE)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_BASE)
        message(FATAL_ERROR "krkrz_merge_vcpkg_manifest: BASE is required")
    endif()
    if(NOT ARG_OUTPUT)
        message(FATAL_ERROR "krkrz_merge_vcpkg_manifest: OUTPUT is required")
    endif()
    if(NOT EXISTS "${ARG_BASE}")
        message(FATAL_ERROR "krkrz_merge_vcpkg_manifest: BASE manifest not found: ${ARG_BASE}")
    endif()

    file(READ "${ARG_BASE}" _merged_json)

    # base の dependencies が無ければ空配列を入れておく
    string(JSON _has_deps ERROR_VARIABLE _err TYPE "${_merged_json}" dependencies)
    if(_err)
        string(JSON _merged_json SET "${_merged_json}" dependencies "[]")
    endif()

    _krkrz_collect_dep_names("${_merged_json}" _seen_names)

    set(_added_log "")

    foreach(_subdir IN LISTS ARG_SUBDIRS)
        if(NOT _subdir)
            continue()
        endif()
        set(_sub_manifest "${_subdir}/vcpkg.json")
        if(NOT EXISTS "${_sub_manifest}")
            continue()
        endif()

        # サブの vcpkg.json が変わったら再 configure
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
            APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sub_manifest}")

        file(READ "${_sub_manifest}" _sub_json)
        string(JSON _sub_deps_count ERROR_VARIABLE _err LENGTH "${_sub_json}" dependencies)
        if(_err OR _sub_deps_count EQUAL 0)
            continue()
        endif()

        get_filename_component(_basename "${_subdir}" NAME)

        set(_added_for_this "")
        set(_excluded_for_this "")
        math(EXPR _last "${_sub_deps_count} - 1")
        foreach(_i RANGE 0 ${_last})
            string(JSON _entry_type TYPE "${_sub_json}" dependencies ${_i})
            string(JSON _entry GET "${_sub_json}" dependencies ${_i})
            if(_entry_type STREQUAL "STRING")
                set(_name "${_entry}")
                set(_value_json "\"${_entry}\"")
            else()
                string(JSON _name GET "${_sub_json}" dependencies ${_i} name)
                set(_value_json "${_entry}")
            endif()
            if("${_name}" IN_LIST ARG_EXCLUDE
                    OR "${_basename}:${_name}" IN_LIST ARG_EXCLUDE)
                list(APPEND _excluded_for_this "${_name}")
                continue()
            endif()
            if(NOT "${_name}" IN_LIST _seen_names)
                string(JSON _cur_count LENGTH "${_merged_json}" dependencies)
                string(JSON _merged_json SET "${_merged_json}"
                    dependencies ${_cur_count} "${_value_json}")
                list(APPEND _seen_names "${_name}")
                list(APPEND _added_for_this "${_name}")
            endif()
        endforeach()
        if(_added_for_this)
            list(JOIN _added_for_this ", " _joined)
            list(APPEND _added_log "  + ${_subdir}: ${_joined}")
        endif()
        if(_excluded_for_this)
            list(JOIN _excluded_for_this ", " _joined)
            list(APPEND _added_log "  - ${_subdir}: excluded ${_joined}")
        endif()
    endforeach()

    # 既存出力と内容が同じなら書き換えない (タイムスタンプ更新による無用な再 install を回避)
    set(_should_write TRUE)
    if(EXISTS "${ARG_OUTPUT}")
        file(READ "${ARG_OUTPUT}" _existing)
        if(_existing STREQUAL _merged_json)
            set(_should_write FALSE)
        endif()
    endif()
    if(_should_write)
        file(WRITE "${ARG_OUTPUT}" "${_merged_json}")
    endif()

    message(STATUS "krkrz_merge_vcpkg_manifest: ${ARG_OUTPUT}")
    foreach(_msg IN LISTS _added_log)
        message(STATUS "${_msg}")
    endforeach()
endfunction()


# 内部ヘルパ: dependencies 配列から name を集めて _out_var に入れる
function(_krkrz_collect_dep_names _json _out_var)
    set(_names "")
    string(JSON _count ERROR_VARIABLE _err LENGTH "${_json}" dependencies)
    if(NOT _err AND _count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_i RANGE 0 ${_last})
            string(JSON _t TYPE "${_json}" dependencies ${_i})
            if(_t STREQUAL "STRING")
                string(JSON _n GET "${_json}" dependencies ${_i})
            else()
                string(JSON _n GET "${_json}" dependencies ${_i} name)
            endif()
            list(APPEND _names "${_n}")
        endforeach()
    endif()
    set(${_out_var} "${_names}" PARENT_SCOPE)
endfunction()
