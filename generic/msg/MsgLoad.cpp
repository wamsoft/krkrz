// generated from gentext.pl Messages.xlsx
#include "tjsCommHead.h"
#include "tjsError.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "MsgLoad.h"
#include "CharacterSet.h"

static bool IS_LOAD_MESSAGE = false;

static tjs_string conv(const std::string &in) {
	tjs_string ret;
	TVPUtf8ToUtf16(ret, in);
	return ret;
}

enum {
	NUM_TJS_INTERNAL_ERROR,
	NUM_TJS_WARNING,
	NUM_TJS_WARN_EVAL_OPERATOR,
	NUM_TJS_NARROW_TO_WIDE_CONVERSION_ERROR,
	NUM_TJS_VARIANT_CONVERT_ERROR,
	NUM_TJS_VARIANT_CONVERT_ERROR_TO_OBJECT,
	NUM_TJS_IDEXPECTED,
	NUM_TJS_SUBSTITUTION_IN_BOOLEAN_CONTEXT,
	NUM_TJS_CANNOT_MODIFY_LHS,
	NUM_TJS_INSUFFICIENT_MEM,
	NUM_TJS_CANNOT_GET_RESULT,
	NUM_TJS_NULL_ACCESS,
	NUM_TJS_MEMBER_NOT_FOUND,
	NUM_TJS_MEMBER_NOT_FOUND_NO_NAME_GIVEN,
	NUM_TJS_NOT_IMPLEMENTED,
	NUM_TJS_INVALID_PARAM,
	NUM_TJS_BAD_PARAM_COUNT,
	NUM_TJS_INVALID_TYPE,
	NUM_TJS_SPECIFY_DIC_OR_ARRAY,
	NUM_TJS_SPECIFY_ARRAY,
	NUM_TJS_STRING_DEALLOC_ERROR,
	NUM_TJS_STRING_ALLOC_ERROR,
	NUM_TJS_MISPLACED_BREAK_CONTINUE,
	NUM_TJS_MISPLACED_CASE,
	NUM_TJS_MISPLACED_RETURN,
	NUM_TJS_STRING_PARSE_ERROR,
	NUM_TJS_NUMBER_ERROR,
	NUM_TJS_UNCLOSED_COMMENT,
	NUM_TJS_INVALID_CHAR,
	NUM_TJS_EXPECTED,
	NUM_TJS_SYNTAX_ERROR,
	NUM_TJS_PPERROR,
	NUM_TJS_CANNOT_GET_SUPER,
	NUM_TJS_INVALID_OPECODE,
	NUM_TJS_RANGE_ERROR,
	NUM_TJS_ACCESS_DENYED,
	NUM_TJS_NATIVE_CLASS_CRASH,
	NUM_TJS_INVALID_OBJECT,
	NUM_TJS_CANNOT_OMIT,
	NUM_TJS_CANNOT_PARSE_DATE,
	NUM_TJS_INVALID_VALUE_FOR_TIMESTAMP,
	NUM_TJS_EXCEPTION_NOT_FOUND,
	NUM_TJS_INVALID_FORMAT_STRING,
	NUM_TJS_DIVIDE_BY_ZERO,
	NUM_TJS_NOT_RECONSTRUCTIVE_RANDOMIZE_DATA,
	NUM_TJS_SYMBOL,
	NUM_TJS_CALL_HISTORY_IS_FROM_OUT_OF_TJS2SCRIPT,
	NUM_TJS_NOBJECTS_WAS_NOT_FREED,
	NUM_TJS_OBJECT_CREATION_HISTORY_DELIMITER_CRLF,
	NUM_TJS_OBJECT_CREATION_HISTORY_DELIMITER,
	NUM_TJS_OBJECT_WAS_NOT_FREED_CRLF,
	NUM_TJS_OBJECT_WAS_NOT_FREED,
	NUM_TJS_GROUP_BY_OBJECT_TYPE_AND_HISTORY,
	NUM_TJS_GROUP_BY_OBJECT_TYPE,
	NUM_TJS_OBJECT_COUNTING_MESSAGE_GROUP_BY_OBJECT_TYPE_AND_HISTORY_CRLF,
	NUM_TJS_OBJECT_COUNTING_MESSAGE_GROUP_BY_OBJECT_TYPE_AND_HISTORY,
	NUM_TJS_OBJECT_COUNTING_MESSAGE_TJSGROUP_BY_OBJECT_TYPE,
	NUM_TJS_WARN_RUNNING_CODE_ON_DELETING_OBJECT_CRLF,
	NUM_TJS_WARN_RUNNING_CODE_ON_DELETING_OBJECT,
	NUM_TJS_WRITE_ERROR,
	NUM_TJS_READ_ERROR,
	NUM_TJS_SEEK_ERROR,
	NUM_TJS_BYTE_CODE_BROKEN,
	NUM_TJS_OBJECT_HASH_MAP_LOG_VERSION_MISMATCH,
	NUM_TJS_CURRUPTED_OBJECT_HASH_MAP_LOG,
	NUM_TJS_UNKNOWN_FAILURE,
	NUM_TJS_UNKNOWN_PACK_UNPACK_TEMPLATE_CHARCTER,
	NUM_TJS_UNKNOWN_BIT_STRING_CHARACTER,
	NUM_TJS_UNKNOWN_HEX_STRING_CHARACTER,
	NUM_TJS_NOT_SUPPORTED_UUENCODE,
	NUM_TJS_NOT_SUPPORTED_BER,
	NUM_TJS_NOT_SUPPORTED_UNPACK_LP,
	NUM_TJS_NOT_SUPPORTED_UNPACK_P,
	NUM_TVP_VERSION_INFORMATION,
	NUM_TVP_VERSION_INFORMATION2,
	NUM_TVP_DOWNLOAD_PAGE_URL,
	NUM_TVP_INTERNAL_ERROR,
	NUM_TVP_INVALID_PARAM,
	NUM_TVP_WARN_DEBUG_OPTION_ENABLED,
	NUM_TVP_COMMAND_LINE_PARAM_IGNORED_AND_DEFAULT_USED,
	NUM_TVP_INVALID_COMMAND_LINE_PARAM,
	NUM_TVP_NOT_IMPLEMENTED,
	NUM_TVP_CANNOT_OPEN_STORAGE,
	NUM_TVP_CANNOT_FIND_STORAGE,
	NUM_TVP_CANNOT_OPEN_STORAGE_FOR_WRITE,
	NUM_TVP_STORAGE_IN_ARCHIVE_NOT_FOUND,
	NUM_TVP_INVALID_PATH_NAME,
	NUM_TVP_UNSUPPORTED_MEDIA_NAME,
	NUM_TVP_CANNOT_UNBIND_XP3EXE,
	NUM_TVP_CANNOT_FIND_XP3MARK,
	NUM_TVP_MISSING_PATH_DELIMITER_AT_LAST,
	NUM_TVP_FILENAME_CONTAINS_SHARP_WARN,
	NUM_TVP_CANNOT_GET_LOCAL_NAME,
	NUM_TVP_READ_ERROR,
	NUM_TVP_WRITE_ERROR,
	NUM_TVP_SEEK_ERROR,
	NUM_TVP_TRUNCATE_ERROR,
	NUM_TVP_INSUFFICIENT_MEMORY,
	NUM_TVP_UNCOMPRESSION_FAILED,
	NUM_TVP_COMPRESSION_FAILED,
	NUM_TVP_CANNOT_WRITE_TO_ARCHIVE,
	NUM_TVP_UNSUPPORTED_CIPHER_MODE,
	NUM_TVP_UNSUPPORTED_ENCODING,
	NUM_TVP_UNSUPPORTED_MODE_STRING,
	NUM_TVP_UNKNOWN_GRAPHIC_FORMAT,
	NUM_TVP_CANNOT_SUGGEST_GRAPHIC_EXTENSION,
	NUM_TVP_MASK_SIZE_MISMATCH,
	NUM_TVP_PROVINCE_SIZE_MISMATCH,
	NUM_TVP_IMAGE_LOAD_ERROR,
	NUM_TVP_JPEGLOAD_ERROR,
	NUM_TVP_PNGLOAD_ERROR,
	NUM_TVP_ERILOAD_ERROR,
	NUM_TVP_TLGLOAD_ERROR,
	NUM_TVP_INVALID_IMAGE_SAVE_TYPE,
	NUM_TVP_INVALID_OPERATION_FOR8BPP,
	NUM_TVP_INVALID_OPERATION_FOR32BPP,
	NUM_TVP_SPECIFY_WINDOW,
	NUM_TVP_SPECIFY_LAYER,
	NUM_TVP_SPECIFY_LAYER_OR_BITMAP,
	NUM_TVP_CANNOT_ACCEPT_MODE_AUTO,
	NUM_TVP_CANNOT_CREATE_EMPTY_LAYER_IMAGE,
	NUM_TVP_CANNOT_SET_PRIMARY_INVISIBLE,
	NUM_TVP_CANNOT_MOVE_PRIMARY,
	NUM_TVP_CANNOT_SET_PARENT_SELF,
	NUM_TVP_CANNOT_MOVE_NEXT_TO_SELF_OR_NOT_SIBLINGS,
	NUM_TVP_CANNOT_MOVE_PRIMARY_OR_SIBLINGLESS,
	NUM_TVP_CANNOT_MOVE_TO_UNDER_OTHER_PRIMARY_LAYER,
	NUM_TVP_INVALID_IMAGE_POSITION,
	NUM_TVP_CANNOT_SET_MODE_TO_DISABLED_OR_MODAL,
	NUM_TVP_NOT_DRAWABLE_LAYER_TYPE,
	NUM_TVP_SOURCE_LAYER_HAS_NO_IMAGE,
	NUM_TVP_UNSUPPORTED_LAYER_TYPE,
	NUM_TVP_NOT_DRAWABLE_FACE_TYPE,
	NUM_TVP_CANNOT_CONVERT_LAYER_TYPE_USING_GIVEN_DIRECTION,
	NUM_TVP_NEGATIVE_OPACITY_NOT_SUPPORTED_ON_THIS_FACE,
	NUM_TVP_SRC_RECT_OUT_OF_BITMAP,
	NUM_TVP_BOX_BLUR_AREA_MUST_CONTAIN_CENTER_PIXEL,
	NUM_TVP_BOX_BLUR_AREA_MUST_BE_SMALLER_THAN16MILLION,
	NUM_TVP_CANNOT_CHANGE_FOCUS_IN_PROCESSING_FOCUS,
	NUM_TVP_WINDOW_HAS_NO_LAYER,
	NUM_TVP_WINDOW_HAS_ALREADY_PRIMARY_LAYER,
	NUM_TVP_SPECIFIED_EVENT_NEEDS_PARAMETER,
	NUM_TVP_SPECIFIED_EVENT_NEEDS_PARAMETER2,
	NUM_TVP_SPECIFIED_EVENT_NAME_IS_UNKNOWN,
	NUM_TVP_OUT_OF_RECTANGLE,
	NUM_TVP_INVALID_METHOD_IN_UPDATING,
	NUM_TVP_CANNOT_CREATE_INSTANCE,
	NUM_TVP_UNKNOWN_WAVE_FORMAT,
	NUM_TVP_CURRENT_TRANSITION_MUST_BE_STOPPING,
	NUM_TVP_TRANS_HANDLER_ERROR,
	NUM_TVP_TRANS_ALREADY_REGISTERED,
	NUM_TVP_CANNOT_FIND_TRANS_HANDER,
	NUM_TVP_SPECIFY_TRANSITION_SOURCE,
	NUM_TVP_LAYER_CANNOT_HAVE_IMAGE,
	NUM_TVP_TRANSITION_SOURCE_AND_DESTINATION_MUST_HAVE_IMAGE,
	NUM_TVP_CANNOT_LOAD_RULE_GRAPHIC,
	NUM_TVP_SPECIFY_OPTION,
	NUM_TVP_TRANSITION_LAYER_SIZE_MISMATCH,
	NUM_TVP_TRANSITION_MUTUAL_SOURCE,
	NUM_TVP_HOLD_DESTINATION_ALPHA_PARAMETER_IS_NOW_DEPRECATED,
	NUM_TVP_CANNOT_CONNECT_MULTIPLE_WAVE_SOUND_BUFFER_AT_ONCE,
	NUM_TVP_INVALID_WINDOW_SIZE_MUST_BE_IN64TO32768,
	NUM_TVP_INVALID_OVERLAP_COUNT_MUST_BE_IN2TO32,
	NUM_TVP_CURRENTLY_ASYNC_LOAD_BITMAP,
	NUM_TVP_FAILD_CLIPBOARD_COPY,
	NUM_TVP_INVALID_UTF16TO_UTF8,
	NUM_TVP_INFO_LOADING_STARTUP_SCRIPT,
	NUM_TVP_INFO_STARTUP_SCRIPT_ENDED,
	NUM_TVP_TJS_CHAR_MUST_BE_TWO_OR_FOUR,
	NUM_TVP_MEDIA_NAME_HAD_ALREADY_BEEN_REGISTERED,
	NUM_TVP_MEDIA_NAME_IS_NOT_REGISTERED,
	NUM_TVP_INFO_REBUILDING_AUTO_PATH,
	NUM_TVP_INFO_TOTAL_FILE_FOUND_AND_ACTIVATED,
	NUM_TVP_ERROR_IN_RETRIEVING_SYSTEM_ON_ACTIVATE_ON_DEACTIVATE,
	NUM_TVP_THE_HOST_IS_NOT_A16BIT_UNICODE_SYSTEM,
	NUM_TVP_INFO_TRYING_TO_READ_XP3VIRTUAL_FILE_SYSTEM_INFORMATION_FROM,
	NUM_TVP_SPECIFIED_STORAGE_HAD_BEEN_PROTECTED,
	NUM_TVP_INFO_FAILED,
	NUM_TVP_INFO_DONE_WITH_CONTAINS,
	NUM_TVP_SEPARATOR_CRLF,
	NUM_TVP_SEPARATOR_CR,
	NUM_TVP_DEFAULT_FONT_NAME,
	NUM_TVP_CANNOT_OPEN_FONT_FILE,
	NUM_TVP_FONT_CANNOT_BE_USED,
	NUM_TVP_FONT_RASTERIZE_ERROR,
	NUM_TVP_BIT_FIELDS_NOT_SUPPORTED,
	NUM_TVP_COMPRESSED_BMP_NOT_SUPPORTED,
	NUM_TVP_UNSUPPORTED_COLOR_MODE_FOR_PALETT_IMAGE,
	NUM_TVP_NOT_WINDOWS_BMP,
	NUM_TVP_UNSUPPORTED_HEADER_VERSION,
	NUM_TVP_INFO_TOUCHING,
	NUM_TVP_ABORTED_TIME_OUT,
	NUM_TVP_ABORTED_LIMIT_BYTE,
	NUM_TVP_FAILD_GLYPH_FOR_DRAW_GLYPH,
	NUM_TVP_LAYER_OBJECT_IS_NOT_PROPERLY_CONSTRUCTED,
	NUM_TVP_UNKNOWN_UPDATE_TYPE,
	NUM_TVP_UNKNOWN_TRANSITION_TYPE,
	NUM_TVP_UNSUPPORTED_UPDATE_TYPE_TUT_GIVE_UPDATE,
	NUM_TVP_ERROR_CODE,
	NUM_TVP_UNSUPPORTED_JPEG_PALETTE,
	NUM_TVP_LIBPNG_ERROR,
	NUM_TVP_UNSUPPORTED_COLOR_TYPE_PALETTE,
	NUM_TVP_UNSUPPORTED_COLOR_TYPE,
	NUM_TVP_TOO_LARGE_IMAGE,
	NUM_TVP_PNG_SAVE_ERROR,
	NUM_TVP_TLG_UNSUPPORTED_UNIVERSAL_TRANSITION_RULE,
	NUM_TVP_UNSUPPORTED_COLOR_COUNT,
	NUM_TVP_DATA_FLAG_MUST_BE_ZERO,
	NUM_TVP_UNSUPPORTED_COLOR_TYPE_COLON,
	NUM_TVP_UNSUPPORTED_EXTERNAL_GOLOMB_BIT_LENGTH_TABLE,
	NUM_TVP_UNSUPPORTED_ENTROPY_CODING_METHOD,
	NUM_TVP_INVALID_TLG_HEADER_OR_VERSION,
	NUM_TVP_TLG_MALFORMED_TAG_MISSION_COLON_AFTER_NAME_LENGTH,
	NUM_TVP_TLG_MALFORMED_TAG_MISSION_EQUALS_AFTER_NAME,
	NUM_TVP_TLG_MALFORMED_TAG_MISSION_COLON_AFTER_VAUE_LENGTH,
	NUM_TVP_TLG_MALFORMED_TAG_MISSION_COMMA_AFTER_TAG,
	NUM_TVP_FILE_SIZE_IS_ZERO,
	NUM_TVP_MEMORY_ALLOCATION_ERROR,
	NUM_TVP_FILE_READ_ERROR,
	NUM_TVP_INVALID_PRERENDERED_FONT_FILE,
	NUM_TVP_NOT16BIT_UNICODE_FONT_FILE,
	NUM_TVP_INVALID_HEADER_VERSION,
	NUM_TVP_TLG_INSUFFICIENT_MEMORY,
	NUM_TVP_TLG_TOO_LARGE_BIT_LENGTH,
	NUM_TVP_CANNOT_RETRIVE_INTERFACE_FROM_DRAW_DEVICE,
	NUM_TVP_ILLEGAL_CHARACTER_CONVERSION_UTF16TO_UTF8,
	NUM_TVP_REQUIRE_LAYER_TREE_OWNER_INTERFACE_INTERFACE,
	NUM_TVP_REQUIRE_SLASH_END_OF_DIRECTORY,
	NUM_TVP_SCRIPT_EXCEPTION_RAISED,
	NUM_TVP_HARDWARE_EXCEPTION_RAISED,
	NUM_TVP_MAIN_CDPNAME,
	NUM_TVP_EXCEPTION_CDPNAME,
	NUM_TVP_CANNNOT_LOCATE_UIDLLFOR_FOLDER_SELECTION,
	NUM_TVP_INVALID_UIDLL,
	NUM_TVP_INVALID_BPP,
	NUM_TVP_CANNOT_LOAD_PLUGIN,
	NUM_TVP_NOT_VALID_PLUGIN,
	NUM_TVP_PLUGIN_UNINIT_FAILED,
	NUM_TVP_CANNNOT_LINK_PLUGIN_WHILE_PLUGIN_LINKING,
	NUM_TVP_NOT_SUSIE_PLUGIN,
	NUM_TVP_SUSIE_PLUGIN_ERROR,
	NUM_TVP_CANNOT_RELEASE_PLUGIN,
	NUM_TVP_NOT_LOADED_PLUGIN,
	NUM_TVP_CANNOT_ALLOCATE_BITMAP_BITS,
	NUM_TVP_SCAN_LINE_RANGE_OVER,
	NUM_TVP_PLUGIN_ERROR,
	NUM_TVP_INVALID_CDDADRIVE,
	NUM_TVP_CDDADRIVE_NOT_FOUND,
	NUM_TVP_MCIERROR,
	NUM_TVP_INVALID_SMF,
	NUM_TVP_MALFORMED_MIDIMESSAGE,
	NUM_TVP_CANNOT_INIT_DIRECT_SOUND,
	NUM_TVP_CANNOT_CREATE_DSSECONDARY_BUFFER,
	NUM_TVP_INVALID_LOOP_INFORMATION,
	NUM_TVP_NOT_CHILD_MENU_ITEM,
	NUM_TVP_CANNOT_INIT_DIRECT3D,
	NUM_TVP_CANNOT_FIND_DISPLAY_MODE,
	NUM_TVP_CANNOT_SWITCH_TO_FULL_SCREEN,
	NUM_TVP_INVALID_PROPERTY_IN_FULL_SCREEN,
	NUM_TVP_INVALID_METHOD_IN_FULL_SCREEN,
	NUM_TVP_CANNOT_LOAD_CURSOR,
	NUM_TVP_CANNOT_LOAD_KR_MOVIE_DLL,
	NUM_TVP_INVALID_KR_MOVIE_DLL,
	NUM_TVP_ERROR_IN_KR_MOVIE_DLL,
	NUM_TVP_WINDOW_ALREADY_MISSING,
	NUM_TVP_PRERENDERED_FONT_MAPPING_FAILED,
	NUM_TVP_CONFIG_FAIL_ORIGINAL_FILE_CANNOT_BE_REWRITTEN,
	NUM_TVP_CONFIG_FAIL_TEMP_EXE_NOT_ERASED,
	NUM_TVP_EXECUTION_FAIL,
	NUM_TVP_PLUGIN_UNBOUND_FUNCTION_ERROR,
	NUM_TVP_EXCEPTION_HAD_BEEN_OCCURED,
	NUM_TVP_CONSOLE_RESULT,
	NUM_TVP_INFO_LISTING_FILES,
	NUM_TVP_INFO_TOTAL_PHYSICAL_MEMORY,
	NUM_TVP_INFO_SELECTED_PROJECT_DIRECTORY,
	NUM_TVP_TOO_SMALL_EXECUTABLE_SIZE,
	NUM_TVP_INFO_LOADING_EXECUTABLE_EMBEDDED_OPTIONS_FAILED,
	NUM_TVP_INFO_LOADING_EXECUTABLE_EMBEDDED_OPTIONS_SUCCEEDED,
	NUM_TVP_FILE_NOT_FOUND,
	NUM_TVP_INFO_LOADING_CONFIGURATION_FILE_FAILED,
	NUM_TVP_INFO_LOADING_CONFIGURATION_FILE_SUCCEEDED,
	NUM_TVP_INFO_DATA_PATH_DOES_NOT_EXIST_TRYING_TO_MAKE_IT,
	NUM_TVP_OK,
	NUM_TVP_FAILD,
	NUM_TVP_INFO_DATA_PATH,
	NUM_TVP_INFO_SPECIFIED_OPTION_EARLIER_ITEM_HAS_MORE_PRIORITY,
	NUM_TVP_NONE,
	NUM_TVP_INFO_CPU_CLOCK_ROUGHLY,
	NUM_TVP_PROGRAM_STARTED_ON,
	NUM_TVP_KIRIKIRI,
	NUM_TVP_UNKNOWN_ERROR,
	NUM_TVP_EXIT_CODE,
	NUM_TVP_FATAL_ERROR,
	NUM_TVP_ENABLE_DIGITIZER,
	NUM_TVP_TOUCH_INTEGRATED_TOUCH,
	NUM_TVP_TOUCH_EXTERNAL_TOUCH,
	NUM_TVP_TOUCH_INTEGRATED_PEN,
	NUM_TVP_TOUCH_EXTERNAL_PEN,
	NUM_TVP_TOUCH_MULTI_INPUT,
	NUM_TVP_TOUCH_READY,
	NUM_TVP_CPU_CHECK_FAILURE,
	NUM_TVP_CPU_CHECK_FAILURE_CPU_FAMILY_OR_LESSER_IS_NOT_SUPPORTED,
	NUM_TVP_INFO_CPU_NUMBER,
	NUM_TVP_CPU_CHECK_FAILURE_NOT_SUPPRTED_CPU,
	NUM_TVP_INFO_FINALLY_DETECTED_CPU_FEATURES,
	NUM_TVP_CPU_CHECK_FAILURE_NOT_SUPPORTED_CPU,
	NUM_TVP_INFO_CPU_CLOCK,
	NUM_TVP_LAYER_BITMAP_BUFFER_UNDERRUN_DETECTED_CHECK_YOUR_DRAWING_CODE,
	NUM_TVP_LAYER_BITMAP_BUFFER_OVERRUN_DETECTED_CHECK_YOUR_DRAWING_CODE,
	NUM_TVP_FAILD_TO_CREATE_DIRECT3D,
	NUM_TVP_FAILD_TO_DECIDE_BACKBUFFER_FORMAT,
	NUM_TVP_FAILD_TO_CREATE_DIRECT3DDEVICE,
	NUM_TVP_FAILD_TO_SET_VIEWPORT,
	NUM_TVP_FAILD_TO_SET_RENDER_STATE,
	NUM_TVP_WARNING_IMAGE_SIZE_TOO_LARGE_MAY_BE_CANNOT_CREATE_TEXTURE,
	NUM_TVP_USE_POWER_OF_TWO_SURFACE,
	NUM_TVP_CANNOT_ALLOCATE_D3DOFF_SCREEN_SURFACE,
	NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICES,
	NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICES_UNKNOWN_REASON,
	NUM_TVP_BASIC_DRAW_DEVICE_TEXTURE_HAS_ALREADY_BEEN_LOCKED,
	NUM_TVP_INTERNAL_ERROR_RESULT,
	NUM_TVP_BASIC_DRAW_DEVICE_INF_POLYGON_DRAWING_FAILED,
	NUM_TVP_BASIC_DRAW_DEVICE_INF_DIRECT3DDEVICE_PRESENT_FAILED,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_RESTART,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_FLAGS,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_PARAM,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_FAILED,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_MODE,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_NOT_UPDATED,
	NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_UNKNOWN_REASON,
	NUM_TVP_FAILED_TO_CREATE_SCREEN_DC,
	NUM_TVP_FAILED_TO_CREATE_OFFSCREEN_BITMAP,
	NUM_TVP_FAILED_TO_CREATE_OFFSCREEN_DC,
	NUM_TVP_INFO_SUSIE_PLUGIN_INFO,
	NUM_TVP_SUSIE_PLUGIN_UNSUPPORTED_BITMAP_HEADER,
	NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICE,
	NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICE_UNKNOWN_REASON,
	NUM_TVP_COULD_NOT_CREATE_ANY_DRAW_DEVICE,
	NUM_TVP_BASIC_DRAW_DEVICE_DOES_NOT_SUPPORTE_LAYER_MANAGER_MORE_THAN_ONE,
	NUM_TVP_INVALID_VIDEO_SIZE,
	NUM_TVP_ROUGH_VSYNC_INTERVAL_READ_FROM_API,
	NUM_TVP_ROUGH_VSYNC_INTERVAL_STILL_SEEMS_WRONG,
	NUM_TVP_INFO_FOUND_DIRECT3DINTERFACE,
	NUM_TVP_INFO_FAILD,
	NUM_TVP_INFO_DIRECT3D,
	NUM_TVP_CANNOT_LOAD_D3DDLL,
	NUM_TVP_NOT_FOUND_DIRECT3DCREATE,
	NUM_TVP_INFO_ENVIRONMENT_USING,
	NUM_TVP_INFO_SEARCH_BEST_FULLSCREEN_RESOLUTION,
	NUM_TVP_INFO_CONDITION_PREFERRED_SCREEN_MODE,
	NUM_TVP_INFO_CONDITION_MODE,
	NUM_TVP_INFO_CONDITION_ZOOM_MODE,
	NUM_TVP_INFO_ENVIRONMENT_DEFAULT_SCREEN_MODE,
	NUM_TVP_INFO_ENVIRONMENT_DEFAULT_SCREEN_ASPECT_RATIO,
	NUM_TVP_INFO_ENVIRONMENT_AVAILABLE_DISPLAY_MODES,
	NUM_TVP_INFO_NOT_FOUND_SCREEN_MODE_FROM_DRIVER,
	NUM_TVP_INFO_RESULT_CANDIDATES,
	NUM_TVP_INFO_TRY_SCREEN_MODE,
	NUM_TVP_ALL_SCREEN_MODE_ERROR,
	NUM_TVP_INFO_CHANGE_SCREEN_MODE_SUCCESS,
	NUM_TVP_SELECT_XP3FILE_OR_FOLDER,
	NUM_TVP_D3D_ERR_DEVICE_LOST,
	NUM_TVP_D3D_ERR_DRIVER_IINTERNAL_ERROR,
	NUM_TVP_D3D_ERR_INVALID_CALL,
	NUM_TVP_D3D_ERR_OUT_OF_VIDEO_MEMORY,
	NUM_TVP_D3D_ERR_OUT_OF_MEMORY,
	NUM_TVP_D3D_ERR_WRONG_TEXTURE_FORMAT,
	NUM_TVP_D3D_ERR_UNSUPORTED_COLOR_OPERATION,
	NUM_TVP_D3D_ERR_UNSUPORTED_COLOR_ARG,
	NUM_TVP_D3D_ERR_UNSUPORTED_AALPHT_OPERATION,
	NUM_TVP_D3D_ERR_UNSUPORTED_ALPHA_ARG,
	NUM_TVP_D3D_ERR_TOO_MANY_OPERATIONS,
	NUM_TVP_D3D_ERR_CONFLICTIONING_TEXTURE_FILTER,
	NUM_TVP_D3D_ERR_UNSUPORTED_FACTOR_VALUE,
	NUM_TVP_D3D_ERR_CONFLICTIONING_RENDER_STATE,
	NUM_TVP_D3D_ERR_UNSUPPORTED_TEXTURE_FILTER,
	NUM_TVP_D3D_ERR_CONFLICTIONING_TEXTURE_PALETTE,
	NUM_TVP_D3D_ERR_NOT_FOUND,
	NUM_TVP_D3D_ERR_MORE_DATA,
	NUM_TVP_D3D_ERR_DEVICE_NOT_RESET,
	NUM_TVP_D3D_ERR_NOT_AVAILABLE,
	NUM_TVP_D3D_ERR_INVALID_DEVICE,
	NUM_TVP_D3D_ERR_DRIVER_INVALID_CALL,
	NUM_TVP_D3D_ERR_WAS_STILL_DRAWING,
	NUM_TVP_D3D_ERR_DEVICE_HUNG,
	NUM_TVP_D3D_ERR_UNSUPPORTED_OVERLAY,
	NUM_TVP_D3D_ERR_UNSUPPORTED_OVERLAY_FORMAT,
	NUM_TVP_D3D_ERR_CANNOT_PROTECT_CONTENT,
	NUM_TVP_D3D_ERR_UNSUPPORTED_CRYPTO,
	NUM_TVP_D3D_ERR_PRESENT_STATISTICS_DIS_JOINT,
	NUM_TVP_D3D_ERR_DEVICE_REMOVED,
	NUM_TVP_D3D_OK_NO_AUTO_GEN,
	NUM_TVP_D3D_ERR_FAIL,
	NUM_TVP_D3D_ERR_INVALID_ARG,
	NUM_TVP_D3D_UNKNOWN_ERROR,
	NUM_TVP_EXCEPTION_ACCESS_VIOLATION,
	NUM_TVP_EXCEPTION_BREAKPOINT,
	NUM_TVP_EXCEPTION_DATATYPE_MISALIGNMENT,
	NUM_TVP_EXCEPTION_SINGLE_STEP,
	NUM_TVP_EXCEPTION_ARRAY_BOUNDS_EXCEEDED,
	NUM_TVP_EXCEPTION_FLT_DENORMAL_OPERAND,
	NUM_TVP_EXCEPTION_FLT_DIVIDE_BY_ZERO,
	NUM_TVP_EXCEPTION_FLT_INEXACT_RESULT,
	NUM_TVP_EXCEPTION_FLT_INVALID_OPERATION,
	NUM_TVP_EXCEPTION_FLT_OVERFLOW,
	NUM_TVP_EXCEPTION_FLT_STACK_CHECK,
	NUM_TVP_EXCEPTION_FLT_UNDERFLOW,
	NUM_TVP_EXCEPTION_INT_DIVIDE_BY_ZERO,
	NUM_TVP_EXCEPTION_INT_OVERFLOW,
	NUM_TVP_EXCEPTION_PRIV_INSTRUCTION,
	NUM_TVP_EXCEPTION_NONCONTINUABLE_EXCEPTION,
	NUM_TVP_EXCEPTION_GUARD_PAGE,
	NUM_TVP_EXCEPTION_ILLEGAL_INSTRUCTION,
	NUM_TVP_EXCEPTION_IN_PAGE_ERROR,
	NUM_TVP_EXCEPTION_INVALID_DISPOSITION,
	NUM_TVP_EXCEPTION_INVALID_HANDLE,
	NUM_TVP_EXCEPTION_STACK_OVERFLOW,
	NUM_TVP_EXCEPTION_UNWIND_CCONSOLIDATE,
	NUM_TVP_CANNOT_SHOW_MODAL_AREADY_SHOWED,
	NUM_TVP_CANNOT_SHOW_MODAL_SINGLE_WINDOW,
	NUM_MESSAGE_MAX
};
void TVPLoadMessage( picojson::array &array ) {
	if( IS_LOAD_MESSAGE ) return;
	IS_LOAD_MESSAGE = true;

	const tjs_char* mes;
	tjs_uint length;
	TJSInternalError.AssignMessage( conv(array[NUM_TJS_INTERNAL_ERROR].get<std::string>()).c_str() );
	TJSWarning.AssignMessage( conv(array[NUM_TJS_WARNING].get<std::string>()).c_str() );
	TJSWarnEvalOperator.AssignMessage( conv(array[NUM_TJS_WARN_EVAL_OPERATOR].get<std::string>()).c_str() );
	TJSNarrowToWideConversionError.AssignMessage( conv(array[NUM_TJS_NARROW_TO_WIDE_CONVERSION_ERROR].get<std::string>()).c_str() );
	TJSVariantConvertError.AssignMessage( conv(array[NUM_TJS_VARIANT_CONVERT_ERROR].get<std::string>()).c_str() );
	TJSVariantConvertErrorToObject.AssignMessage( conv(array[NUM_TJS_VARIANT_CONVERT_ERROR_TO_OBJECT].get<std::string>()).c_str() );
	TJSIDExpected.AssignMessage( conv(array[NUM_TJS_IDEXPECTED].get<std::string>()).c_str() );
	TJSSubstitutionInBooleanContext.AssignMessage( conv(array[NUM_TJS_SUBSTITUTION_IN_BOOLEAN_CONTEXT].get<std::string>()).c_str() );
	TJSCannotModifyLHS.AssignMessage( conv(array[NUM_TJS_CANNOT_MODIFY_LHS].get<std::string>()).c_str() );
	TJSInsufficientMem.AssignMessage( conv(array[NUM_TJS_INSUFFICIENT_MEM].get<std::string>()).c_str() );
	TJSCannotGetResult.AssignMessage( conv(array[NUM_TJS_CANNOT_GET_RESULT].get<std::string>()).c_str() );
	TJSNullAccess.AssignMessage( conv(array[NUM_TJS_NULL_ACCESS].get<std::string>()).c_str() );
	TJSMemberNotFound.AssignMessage( conv(array[NUM_TJS_MEMBER_NOT_FOUND].get<std::string>()).c_str() );
	TJSMemberNotFoundNoNameGiven.AssignMessage( conv(array[NUM_TJS_MEMBER_NOT_FOUND_NO_NAME_GIVEN].get<std::string>()).c_str() );
	TJSNotImplemented.AssignMessage( conv(array[NUM_TJS_NOT_IMPLEMENTED].get<std::string>()).c_str() );
	TJSInvalidParam.AssignMessage( conv(array[NUM_TJS_INVALID_PARAM].get<std::string>()).c_str() );
	TJSBadParamCount.AssignMessage( conv(array[NUM_TJS_BAD_PARAM_COUNT].get<std::string>()).c_str() );
	TJSInvalidType.AssignMessage( conv(array[NUM_TJS_INVALID_TYPE].get<std::string>()).c_str() );
	TJSSpecifyDicOrArray.AssignMessage( conv(array[NUM_TJS_SPECIFY_DIC_OR_ARRAY].get<std::string>()).c_str() );
	TJSSpecifyArray.AssignMessage( conv(array[NUM_TJS_SPECIFY_ARRAY].get<std::string>()).c_str() );
	TJSStringDeallocError.AssignMessage( conv(array[NUM_TJS_STRING_DEALLOC_ERROR].get<std::string>()).c_str() );
	TJSStringAllocError.AssignMessage( conv(array[NUM_TJS_STRING_ALLOC_ERROR].get<std::string>()).c_str() );
	TJSMisplacedBreakContinue.AssignMessage( conv(array[NUM_TJS_MISPLACED_BREAK_CONTINUE].get<std::string>()).c_str() );
	TJSMisplacedCase.AssignMessage( conv(array[NUM_TJS_MISPLACED_CASE].get<std::string>()).c_str() );
	TJSMisplacedReturn.AssignMessage( conv(array[NUM_TJS_MISPLACED_RETURN].get<std::string>()).c_str() );
	TJSStringParseError.AssignMessage( conv(array[NUM_TJS_STRING_PARSE_ERROR].get<std::string>()).c_str() );
	TJSNumberError.AssignMessage( conv(array[NUM_TJS_NUMBER_ERROR].get<std::string>()).c_str() );
	TJSUnclosedComment.AssignMessage( conv(array[NUM_TJS_UNCLOSED_COMMENT].get<std::string>()).c_str() );
	TJSInvalidChar.AssignMessage( conv(array[NUM_TJS_INVALID_CHAR].get<std::string>()).c_str() );
	TJSExpected.AssignMessage( conv(array[NUM_TJS_EXPECTED].get<std::string>()).c_str() );
	TJSSyntaxError.AssignMessage( conv(array[NUM_TJS_SYNTAX_ERROR].get<std::string>()).c_str() );
	TJSPPError.AssignMessage( conv(array[NUM_TJS_PPERROR].get<std::string>()).c_str() );
	TJSCannotGetSuper.AssignMessage( conv(array[NUM_TJS_CANNOT_GET_SUPER].get<std::string>()).c_str() );
	TJSInvalidOpecode.AssignMessage( conv(array[NUM_TJS_INVALID_OPECODE].get<std::string>()).c_str() );
	TJSRangeError.AssignMessage( conv(array[NUM_TJS_RANGE_ERROR].get<std::string>()).c_str() );
	TJSAccessDenyed.AssignMessage( conv(array[NUM_TJS_ACCESS_DENYED].get<std::string>()).c_str() );
	TJSNativeClassCrash.AssignMessage( conv(array[NUM_TJS_NATIVE_CLASS_CRASH].get<std::string>()).c_str() );
	TJSInvalidObject.AssignMessage( conv(array[NUM_TJS_INVALID_OBJECT].get<std::string>()).c_str() );
	TJSCannotOmit.AssignMessage( conv(array[NUM_TJS_CANNOT_OMIT].get<std::string>()).c_str() );
	TJSCannotParseDate.AssignMessage( conv(array[NUM_TJS_CANNOT_PARSE_DATE].get<std::string>()).c_str() );
	TJSInvalidValueForTimestamp.AssignMessage( conv(array[NUM_TJS_INVALID_VALUE_FOR_TIMESTAMP].get<std::string>()).c_str() );
	TJSExceptionNotFound.AssignMessage( conv(array[NUM_TJS_EXCEPTION_NOT_FOUND].get<std::string>()).c_str() );
	TJSInvalidFormatString.AssignMessage( conv(array[NUM_TJS_INVALID_FORMAT_STRING].get<std::string>()).c_str() );
	TJSDivideByZero.AssignMessage( conv(array[NUM_TJS_DIVIDE_BY_ZERO].get<std::string>()).c_str() );
	TJSNotReconstructiveRandomizeData.AssignMessage( conv(array[NUM_TJS_NOT_RECONSTRUCTIVE_RANDOMIZE_DATA].get<std::string>()).c_str() );
	TJSSymbol.AssignMessage( conv(array[NUM_TJS_SYMBOL].get<std::string>()).c_str() );
	TJSCallHistoryIsFromOutOfTJS2Script.AssignMessage( conv(array[NUM_TJS_CALL_HISTORY_IS_FROM_OUT_OF_TJS2SCRIPT].get<std::string>()).c_str() );
	TJSNObjectsWasNotFreed.AssignMessage( conv(array[NUM_TJS_NOBJECTS_WAS_NOT_FREED].get<std::string>()).c_str() );
	TJSObjectCreationHistoryDelimiter.AssignMessage( conv(array[NUM_TJS_OBJECT_CREATION_HISTORY_DELIMITER_CRLF].get<std::string>()).c_str() );
	TJSObjectCreationHistoryDelimiter.AssignMessage( conv(array[NUM_TJS_OBJECT_CREATION_HISTORY_DELIMITER].get<std::string>()).c_str() );
	TJSObjectWasNotFreed.AssignMessage( conv(array[NUM_TJS_OBJECT_WAS_NOT_FREED_CRLF].get<std::string>()).c_str() );
	TJSObjectWasNotFreed.AssignMessage( conv(array[NUM_TJS_OBJECT_WAS_NOT_FREED].get<std::string>()).c_str() );
	TJSGroupByObjectTypeAndHistory.AssignMessage( conv(array[NUM_TJS_GROUP_BY_OBJECT_TYPE_AND_HISTORY].get<std::string>()).c_str() );
	TJSGroupByObjectType.AssignMessage( conv(array[NUM_TJS_GROUP_BY_OBJECT_TYPE].get<std::string>()).c_str() );
	TJSObjectCountingMessageGroupByObjectTypeAndHistory.AssignMessage( conv(array[NUM_TJS_OBJECT_COUNTING_MESSAGE_GROUP_BY_OBJECT_TYPE_AND_HISTORY_CRLF].get<std::string>()).c_str() );
	TJSObjectCountingMessageGroupByObjectTypeAndHistory.AssignMessage( conv(array[NUM_TJS_OBJECT_COUNTING_MESSAGE_GROUP_BY_OBJECT_TYPE_AND_HISTORY].get<std::string>()).c_str() );
	TJSObjectCountingMessageTJSGroupByObjectType.AssignMessage( conv(array[NUM_TJS_OBJECT_COUNTING_MESSAGE_TJSGROUP_BY_OBJECT_TYPE].get<std::string>()).c_str() );
	TJSWarnRunningCodeOnDeletingObject.AssignMessage( conv(array[NUM_TJS_WARN_RUNNING_CODE_ON_DELETING_OBJECT_CRLF].get<std::string>()).c_str() );
	TJSWarnRunningCodeOnDeletingObject.AssignMessage( conv(array[NUM_TJS_WARN_RUNNING_CODE_ON_DELETING_OBJECT].get<std::string>()).c_str() );
	TJSWriteError.AssignMessage( conv(array[NUM_TJS_WRITE_ERROR].get<std::string>()).c_str() );
	TJSReadError.AssignMessage( conv(array[NUM_TJS_READ_ERROR].get<std::string>()).c_str() );
	TJSSeekError.AssignMessage( conv(array[NUM_TJS_SEEK_ERROR].get<std::string>()).c_str() );
	TJSByteCodeBroken.AssignMessage( conv(array[NUM_TJS_BYTE_CODE_BROKEN].get<std::string>()).c_str() );
	TJSObjectHashMapLogVersionMismatch.AssignMessage( conv(array[NUM_TJS_OBJECT_HASH_MAP_LOG_VERSION_MISMATCH].get<std::string>()).c_str() );
	TJSCurruptedObjectHashMapLog.AssignMessage( conv(array[NUM_TJS_CURRUPTED_OBJECT_HASH_MAP_LOG].get<std::string>()).c_str() );
	TJSUnknownFailure.AssignMessage( conv(array[NUM_TJS_UNKNOWN_FAILURE].get<std::string>()).c_str() );
	TJSUnknownPackUnpackTemplateCharcter.AssignMessage( conv(array[NUM_TJS_UNKNOWN_PACK_UNPACK_TEMPLATE_CHARCTER].get<std::string>()).c_str() );
	TJSUnknownBitStringCharacter.AssignMessage( conv(array[NUM_TJS_UNKNOWN_BIT_STRING_CHARACTER].get<std::string>()).c_str() );
	TJSUnknownHexStringCharacter.AssignMessage( conv(array[NUM_TJS_UNKNOWN_HEX_STRING_CHARACTER].get<std::string>()).c_str() );
	TJSNotSupportedUuencode.AssignMessage( conv(array[NUM_TJS_NOT_SUPPORTED_UUENCODE].get<std::string>()).c_str() );
	TJSNotSupportedBER.AssignMessage( conv(array[NUM_TJS_NOT_SUPPORTED_BER].get<std::string>()).c_str() );
	TJSNotSupportedUnpackLP.AssignMessage( conv(array[NUM_TJS_NOT_SUPPORTED_UNPACK_LP].get<std::string>()).c_str() );
	TJSNotSupportedUnpackP.AssignMessage( conv(array[NUM_TJS_NOT_SUPPORTED_UNPACK_P].get<std::string>()).c_str() );
	TVPVersionInformation.AssignMessage( conv(array[NUM_TVP_VERSION_INFORMATION].get<std::string>()).c_str() );
	TVPVersionInformation2.AssignMessage( conv(array[NUM_TVP_VERSION_INFORMATION2].get<std::string>()).c_str() );
	TVPDownloadPageURL.AssignMessage( conv(array[NUM_TVP_DOWNLOAD_PAGE_URL].get<std::string>()).c_str() );
	TVPInternalError.AssignMessage( conv(array[NUM_TVP_INTERNAL_ERROR].get<std::string>()).c_str() );
	TVPInvalidParam.AssignMessage( conv(array[NUM_TVP_INVALID_PARAM].get<std::string>()).c_str() );
	TVPWarnDebugOptionEnabled.AssignMessage( conv(array[NUM_TVP_WARN_DEBUG_OPTION_ENABLED].get<std::string>()).c_str() );
	TVPCommandLineParamIgnoredAndDefaultUsed.AssignMessage( conv(array[NUM_TVP_COMMAND_LINE_PARAM_IGNORED_AND_DEFAULT_USED].get<std::string>()).c_str() );
	TVPInvalidCommandLineParam.AssignMessage( conv(array[NUM_TVP_INVALID_COMMAND_LINE_PARAM].get<std::string>()).c_str() );
	TVPNotImplemented.AssignMessage( conv(array[NUM_TVP_NOT_IMPLEMENTED].get<std::string>()).c_str() );
	TVPCannotOpenStorage.AssignMessage( conv(array[NUM_TVP_CANNOT_OPEN_STORAGE].get<std::string>()).c_str() );
	TVPCannotFindStorage.AssignMessage( conv(array[NUM_TVP_CANNOT_FIND_STORAGE].get<std::string>()).c_str() );
	TVPCannotOpenStorageForWrite.AssignMessage( conv(array[NUM_TVP_CANNOT_OPEN_STORAGE_FOR_WRITE].get<std::string>()).c_str() );
	TVPStorageInArchiveNotFound.AssignMessage( conv(array[NUM_TVP_STORAGE_IN_ARCHIVE_NOT_FOUND].get<std::string>()).c_str() );
	TVPInvalidPathName.AssignMessage( conv(array[NUM_TVP_INVALID_PATH_NAME].get<std::string>()).c_str() );
	TVPUnsupportedMediaName.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_MEDIA_NAME].get<std::string>()).c_str() );
	TVPCannotUnbindXP3EXE.AssignMessage( conv(array[NUM_TVP_CANNOT_UNBIND_XP3EXE].get<std::string>()).c_str() );
	TVPCannotFindXP3Mark.AssignMessage( conv(array[NUM_TVP_CANNOT_FIND_XP3MARK].get<std::string>()).c_str() );
	TVPMissingPathDelimiterAtLast.AssignMessage( conv(array[NUM_TVP_MISSING_PATH_DELIMITER_AT_LAST].get<std::string>()).c_str() );
	TVPFilenameContainsSharpWarn.AssignMessage( conv(array[NUM_TVP_FILENAME_CONTAINS_SHARP_WARN].get<std::string>()).c_str() );
	TVPCannotGetLocalName.AssignMessage( conv(array[NUM_TVP_CANNOT_GET_LOCAL_NAME].get<std::string>()).c_str() );
	TVPReadError.AssignMessage( conv(array[NUM_TVP_READ_ERROR].get<std::string>()).c_str() );
	TVPWriteError.AssignMessage( conv(array[NUM_TVP_WRITE_ERROR].get<std::string>()).c_str() );
	TVPSeekError.AssignMessage( conv(array[NUM_TVP_SEEK_ERROR].get<std::string>()).c_str() );
	TVPTruncateError.AssignMessage( conv(array[NUM_TVP_TRUNCATE_ERROR].get<std::string>()).c_str() );
	TVPInsufficientMemory.AssignMessage( conv(array[NUM_TVP_INSUFFICIENT_MEMORY].get<std::string>()).c_str() );
	TVPUncompressionFailed.AssignMessage( conv(array[NUM_TVP_UNCOMPRESSION_FAILED].get<std::string>()).c_str() );
	TVPCompressionFailed.AssignMessage( conv(array[NUM_TVP_COMPRESSION_FAILED].get<std::string>()).c_str() );
	TVPCannotWriteToArchive.AssignMessage( conv(array[NUM_TVP_CANNOT_WRITE_TO_ARCHIVE].get<std::string>()).c_str() );
	TVPUnsupportedCipherMode.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_CIPHER_MODE].get<std::string>()).c_str() );
	TVPUnsupportedEncoding.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_ENCODING].get<std::string>()).c_str() );
	TVPUnsupportedModeString.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_MODE_STRING].get<std::string>()).c_str() );
	TVPUnknownGraphicFormat.AssignMessage( conv(array[NUM_TVP_UNKNOWN_GRAPHIC_FORMAT].get<std::string>()).c_str() );
	TVPCannotSuggestGraphicExtension.AssignMessage( conv(array[NUM_TVP_CANNOT_SUGGEST_GRAPHIC_EXTENSION].get<std::string>()).c_str() );
	TVPMaskSizeMismatch.AssignMessage( conv(array[NUM_TVP_MASK_SIZE_MISMATCH].get<std::string>()).c_str() );
	TVPProvinceSizeMismatch.AssignMessage( conv(array[NUM_TVP_PROVINCE_SIZE_MISMATCH].get<std::string>()).c_str() );
	TVPImageLoadError.AssignMessage( conv(array[NUM_TVP_IMAGE_LOAD_ERROR].get<std::string>()).c_str() );
	TVPJPEGLoadError.AssignMessage( conv(array[NUM_TVP_JPEGLOAD_ERROR].get<std::string>()).c_str() );
	TVPPNGLoadError.AssignMessage( conv(array[NUM_TVP_PNGLOAD_ERROR].get<std::string>()).c_str() );
	TVPERILoadError.AssignMessage( conv(array[NUM_TVP_ERILOAD_ERROR].get<std::string>()).c_str() );
	TVPTLGLoadError.AssignMessage( conv(array[NUM_TVP_TLGLOAD_ERROR].get<std::string>()).c_str() );
	TVPInvalidImageSaveType.AssignMessage( conv(array[NUM_TVP_INVALID_IMAGE_SAVE_TYPE].get<std::string>()).c_str() );
	TVPInvalidOperationFor8BPP.AssignMessage( conv(array[NUM_TVP_INVALID_OPERATION_FOR8BPP].get<std::string>()).c_str() );
	TVPInvalidOperationFor32BPP.AssignMessage( conv(array[NUM_TVP_INVALID_OPERATION_FOR32BPP].get<std::string>()).c_str() );
	TVPSpecifyWindow.AssignMessage( conv(array[NUM_TVP_SPECIFY_WINDOW].get<std::string>()).c_str() );
	TVPSpecifyLayer.AssignMessage( conv(array[NUM_TVP_SPECIFY_LAYER].get<std::string>()).c_str() );
	TVPSpecifyLayerOrBitmap.AssignMessage( conv(array[NUM_TVP_SPECIFY_LAYER_OR_BITMAP].get<std::string>()).c_str() );
	TVPCannotAcceptModeAuto.AssignMessage( conv(array[NUM_TVP_CANNOT_ACCEPT_MODE_AUTO].get<std::string>()).c_str() );
	TVPCannotCreateEmptyLayerImage.AssignMessage( conv(array[NUM_TVP_CANNOT_CREATE_EMPTY_LAYER_IMAGE].get<std::string>()).c_str() );
	TVPCannotSetPrimaryInvisible.AssignMessage( conv(array[NUM_TVP_CANNOT_SET_PRIMARY_INVISIBLE].get<std::string>()).c_str() );
	TVPCannotMovePrimary.AssignMessage( conv(array[NUM_TVP_CANNOT_MOVE_PRIMARY].get<std::string>()).c_str() );
	TVPCannotSetParentSelf.AssignMessage( conv(array[NUM_TVP_CANNOT_SET_PARENT_SELF].get<std::string>()).c_str() );
	TVPCannotMoveNextToSelfOrNotSiblings.AssignMessage( conv(array[NUM_TVP_CANNOT_MOVE_NEXT_TO_SELF_OR_NOT_SIBLINGS].get<std::string>()).c_str() );
	TVPCannotMovePrimaryOrSiblingless.AssignMessage( conv(array[NUM_TVP_CANNOT_MOVE_PRIMARY_OR_SIBLINGLESS].get<std::string>()).c_str() );
	TVPCannotMoveToUnderOtherPrimaryLayer.AssignMessage( conv(array[NUM_TVP_CANNOT_MOVE_TO_UNDER_OTHER_PRIMARY_LAYER].get<std::string>()).c_str() );
	TVPInvalidImagePosition.AssignMessage( conv(array[NUM_TVP_INVALID_IMAGE_POSITION].get<std::string>()).c_str() );
	TVPCannotSetModeToDisabledOrModal.AssignMessage( conv(array[NUM_TVP_CANNOT_SET_MODE_TO_DISABLED_OR_MODAL].get<std::string>()).c_str() );
	TVPNotDrawableLayerType.AssignMessage( conv(array[NUM_TVP_NOT_DRAWABLE_LAYER_TYPE].get<std::string>()).c_str() );
	TVPSourceLayerHasNoImage.AssignMessage( conv(array[NUM_TVP_SOURCE_LAYER_HAS_NO_IMAGE].get<std::string>()).c_str() );
	TVPUnsupportedLayerType.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_LAYER_TYPE].get<std::string>()).c_str() );
	TVPNotDrawableFaceType.AssignMessage( conv(array[NUM_TVP_NOT_DRAWABLE_FACE_TYPE].get<std::string>()).c_str() );
	TVPCannotConvertLayerTypeUsingGivenDirection.AssignMessage( conv(array[NUM_TVP_CANNOT_CONVERT_LAYER_TYPE_USING_GIVEN_DIRECTION].get<std::string>()).c_str() );
	TVPNegativeOpacityNotSupportedOnThisFace.AssignMessage( conv(array[NUM_TVP_NEGATIVE_OPACITY_NOT_SUPPORTED_ON_THIS_FACE].get<std::string>()).c_str() );
	TVPSrcRectOutOfBitmap.AssignMessage( conv(array[NUM_TVP_SRC_RECT_OUT_OF_BITMAP].get<std::string>()).c_str() );
	TVPBoxBlurAreaMustContainCenterPixel.AssignMessage( conv(array[NUM_TVP_BOX_BLUR_AREA_MUST_CONTAIN_CENTER_PIXEL].get<std::string>()).c_str() );
	TVPBoxBlurAreaMustBeSmallerThan16Million.AssignMessage( conv(array[NUM_TVP_BOX_BLUR_AREA_MUST_BE_SMALLER_THAN16MILLION].get<std::string>()).c_str() );
	TVPCannotChangeFocusInProcessingFocus.AssignMessage( conv(array[NUM_TVP_CANNOT_CHANGE_FOCUS_IN_PROCESSING_FOCUS].get<std::string>()).c_str() );
	TVPWindowHasNoLayer.AssignMessage( conv(array[NUM_TVP_WINDOW_HAS_NO_LAYER].get<std::string>()).c_str() );
	TVPWindowHasAlreadyPrimaryLayer.AssignMessage( conv(array[NUM_TVP_WINDOW_HAS_ALREADY_PRIMARY_LAYER].get<std::string>()).c_str() );
	TVPSpecifiedEventNeedsParameter.AssignMessage( conv(array[NUM_TVP_SPECIFIED_EVENT_NEEDS_PARAMETER].get<std::string>()).c_str() );
	TVPSpecifiedEventNeedsParameter2.AssignMessage( conv(array[NUM_TVP_SPECIFIED_EVENT_NEEDS_PARAMETER2].get<std::string>()).c_str() );
	TVPSpecifiedEventNameIsUnknown.AssignMessage( conv(array[NUM_TVP_SPECIFIED_EVENT_NAME_IS_UNKNOWN].get<std::string>()).c_str() );
	TVPOutOfRectangle.AssignMessage( conv(array[NUM_TVP_OUT_OF_RECTANGLE].get<std::string>()).c_str() );
	TVPInvalidMethodInUpdating.AssignMessage( conv(array[NUM_TVP_INVALID_METHOD_IN_UPDATING].get<std::string>()).c_str() );
	TVPCannotCreateInstance.AssignMessage( conv(array[NUM_TVP_CANNOT_CREATE_INSTANCE].get<std::string>()).c_str() );
	TVPUnknownWaveFormat.AssignMessage( conv(array[NUM_TVP_UNKNOWN_WAVE_FORMAT].get<std::string>()).c_str() );
	TVPCurrentTransitionMustBeStopping.AssignMessage( conv(array[NUM_TVP_CURRENT_TRANSITION_MUST_BE_STOPPING].get<std::string>()).c_str() );
	TVPTransHandlerError.AssignMessage( conv(array[NUM_TVP_TRANS_HANDLER_ERROR].get<std::string>()).c_str() );
	TVPTransAlreadyRegistered.AssignMessage( conv(array[NUM_TVP_TRANS_ALREADY_REGISTERED].get<std::string>()).c_str() );
	TVPCannotFindTransHander.AssignMessage( conv(array[NUM_TVP_CANNOT_FIND_TRANS_HANDER].get<std::string>()).c_str() );
	TVPSpecifyTransitionSource.AssignMessage( conv(array[NUM_TVP_SPECIFY_TRANSITION_SOURCE].get<std::string>()).c_str() );
	TVPLayerCannotHaveImage.AssignMessage( conv(array[NUM_TVP_LAYER_CANNOT_HAVE_IMAGE].get<std::string>()).c_str() );
	TVPTransitionSourceAndDestinationMustHaveImage.AssignMessage( conv(array[NUM_TVP_TRANSITION_SOURCE_AND_DESTINATION_MUST_HAVE_IMAGE].get<std::string>()).c_str() );
	TVPCannotLoadRuleGraphic.AssignMessage( conv(array[NUM_TVP_CANNOT_LOAD_RULE_GRAPHIC].get<std::string>()).c_str() );
	TVPSpecifyOption.AssignMessage( conv(array[NUM_TVP_SPECIFY_OPTION].get<std::string>()).c_str() );
	TVPTransitionLayerSizeMismatch.AssignMessage( conv(array[NUM_TVP_TRANSITION_LAYER_SIZE_MISMATCH].get<std::string>()).c_str() );
	TVPTransitionMutualSource.AssignMessage( conv(array[NUM_TVP_TRANSITION_MUTUAL_SOURCE].get<std::string>()).c_str() );
	TVPHoldDestinationAlphaParameterIsNowDeprecated.AssignMessage( conv(array[NUM_TVP_HOLD_DESTINATION_ALPHA_PARAMETER_IS_NOW_DEPRECATED].get<std::string>()).c_str() );
	TVPCannotConnectMultipleWaveSoundBufferAtOnce.AssignMessage( conv(array[NUM_TVP_CANNOT_CONNECT_MULTIPLE_WAVE_SOUND_BUFFER_AT_ONCE].get<std::string>()).c_str() );
	TVPInvalidWindowSizeMustBeIn64to32768.AssignMessage( conv(array[NUM_TVP_INVALID_WINDOW_SIZE_MUST_BE_IN64TO32768].get<std::string>()).c_str() );
	TVPInvalidOverlapCountMustBeIn2to32.AssignMessage( conv(array[NUM_TVP_INVALID_OVERLAP_COUNT_MUST_BE_IN2TO32].get<std::string>()).c_str() );
	TVPCurrentlyAsyncLoadBitmap.AssignMessage( conv(array[NUM_TVP_CURRENTLY_ASYNC_LOAD_BITMAP].get<std::string>()).c_str() );
	TVPFaildClipboardCopy.AssignMessage( conv(array[NUM_TVP_FAILD_CLIPBOARD_COPY].get<std::string>()).c_str() );
	TVPInvalidUTF16ToUTF8.AssignMessage( conv(array[NUM_TVP_INVALID_UTF16TO_UTF8].get<std::string>()).c_str() );
	TVPInfoLoadingStartupScript.AssignMessage( conv(array[NUM_TVP_INFO_LOADING_STARTUP_SCRIPT].get<std::string>()).c_str() );
	TVPInfoStartupScriptEnded.AssignMessage( conv(array[NUM_TVP_INFO_STARTUP_SCRIPT_ENDED].get<std::string>()).c_str() );
	TVPTjsCharMustBeTwoOrFour.AssignMessage( conv(array[NUM_TVP_TJS_CHAR_MUST_BE_TWO_OR_FOUR].get<std::string>()).c_str() );
	TVPMediaNameHadAlreadyBeenRegistered.AssignMessage( conv(array[NUM_TVP_MEDIA_NAME_HAD_ALREADY_BEEN_REGISTERED].get<std::string>()).c_str() );
	TVPMediaNameIsNotRegistered.AssignMessage( conv(array[NUM_TVP_MEDIA_NAME_IS_NOT_REGISTERED].get<std::string>()).c_str() );
	TVPInfoRebuildingAutoPath.AssignMessage( conv(array[NUM_TVP_INFO_REBUILDING_AUTO_PATH].get<std::string>()).c_str() );
	TVPInfoTotalFileFoundAndActivated.AssignMessage( conv(array[NUM_TVP_INFO_TOTAL_FILE_FOUND_AND_ACTIVATED].get<std::string>()).c_str() );
	TVPErrorInRetrievingSystemOnActivateOnDeactivate.AssignMessage( conv(array[NUM_TVP_ERROR_IN_RETRIEVING_SYSTEM_ON_ACTIVATE_ON_DEACTIVATE].get<std::string>()).c_str() );
	TVPTheHostIsNotA16BitUnicodeSystem.AssignMessage( conv(array[NUM_TVP_THE_HOST_IS_NOT_A16BIT_UNICODE_SYSTEM].get<std::string>()).c_str() );
	TVPInfoTryingToReadXp3VirtualFileSystemInformationFrom.AssignMessage( conv(array[NUM_TVP_INFO_TRYING_TO_READ_XP3VIRTUAL_FILE_SYSTEM_INFORMATION_FROM].get<std::string>()).c_str() );
	TVPSpecifiedStorageHadBeenProtected.AssignMessage( conv(array[NUM_TVP_SPECIFIED_STORAGE_HAD_BEEN_PROTECTED].get<std::string>()).c_str() );
	TVPInfoFailed.AssignMessage( conv(array[NUM_TVP_INFO_FAILED].get<std::string>()).c_str() );
	TVPInfoDoneWithContains.AssignMessage( conv(array[NUM_TVP_INFO_DONE_WITH_CONTAINS].get<std::string>()).c_str() );
	TVPSeparatorCRLF.AssignMessage( conv(array[NUM_TVP_SEPARATOR_CRLF].get<std::string>()).c_str() );
	TVPSeparatorCR.AssignMessage( conv(array[NUM_TVP_SEPARATOR_CR].get<std::string>()).c_str() );
	TVPDefaultFontName.AssignMessage( conv(array[NUM_TVP_DEFAULT_FONT_NAME].get<std::string>()).c_str() );
	TVPCannotOpenFontFile.AssignMessage( conv(array[NUM_TVP_CANNOT_OPEN_FONT_FILE].get<std::string>()).c_str() );
	TVPFontCannotBeUsed.AssignMessage( conv(array[NUM_TVP_FONT_CANNOT_BE_USED].get<std::string>()).c_str() );
	TVPFontRasterizeError.AssignMessage( conv(array[NUM_TVP_FONT_RASTERIZE_ERROR].get<std::string>()).c_str() );
	TVPBitFieldsNotSupported.AssignMessage( conv(array[NUM_TVP_BIT_FIELDS_NOT_SUPPORTED].get<std::string>()).c_str() );
	TVPCompressedBmpNotSupported.AssignMessage( conv(array[NUM_TVP_COMPRESSED_BMP_NOT_SUPPORTED].get<std::string>()).c_str() );
	TVPUnsupportedColorModeForPalettImage.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_COLOR_MODE_FOR_PALETT_IMAGE].get<std::string>()).c_str() );
	TVPNotWindowsBmp.AssignMessage( conv(array[NUM_TVP_NOT_WINDOWS_BMP].get<std::string>()).c_str() );
	TVPUnsupportedHeaderVersion.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_HEADER_VERSION].get<std::string>()).c_str() );
	TVPInfoTouching.AssignMessage( conv(array[NUM_TVP_INFO_TOUCHING].get<std::string>()).c_str() );
	TVPAbortedTimeOut.AssignMessage( conv(array[NUM_TVP_ABORTED_TIME_OUT].get<std::string>()).c_str() );
	TVPAbortedLimitByte.AssignMessage( conv(array[NUM_TVP_ABORTED_LIMIT_BYTE].get<std::string>()).c_str() );
	TVPFaildGlyphForDrawGlyph.AssignMessage( conv(array[NUM_TVP_FAILD_GLYPH_FOR_DRAW_GLYPH].get<std::string>()).c_str() );
	TVPLayerObjectIsNotProperlyConstructed.AssignMessage( conv(array[NUM_TVP_LAYER_OBJECT_IS_NOT_PROPERLY_CONSTRUCTED].get<std::string>()).c_str() );
	TVPUnknownUpdateType.AssignMessage( conv(array[NUM_TVP_UNKNOWN_UPDATE_TYPE].get<std::string>()).c_str() );
	TVPUnknownTransitionType.AssignMessage( conv(array[NUM_TVP_UNKNOWN_TRANSITION_TYPE].get<std::string>()).c_str() );
	TVPUnsupportedUpdateTypeTutGiveUpdate.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_UPDATE_TYPE_TUT_GIVE_UPDATE].get<std::string>()).c_str() );
	TVPErrorCode.AssignMessage( conv(array[NUM_TVP_ERROR_CODE].get<std::string>()).c_str() );
	TVPUnsupportedJpegPalette.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_JPEG_PALETTE].get<std::string>()).c_str() );
	TVPLibpngError.AssignMessage( conv(array[NUM_TVP_LIBPNG_ERROR].get<std::string>()).c_str() );
	TVPUnsupportedColorTypePalette.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_COLOR_TYPE_PALETTE].get<std::string>()).c_str() );
	TVPUnsupportedColorType.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_COLOR_TYPE].get<std::string>()).c_str() );
	TVPTooLargeImage.AssignMessage( conv(array[NUM_TVP_TOO_LARGE_IMAGE].get<std::string>()).c_str() );
	TVPPngSaveError.AssignMessage( conv(array[NUM_TVP_PNG_SAVE_ERROR].get<std::string>()).c_str() );
	TVPTlgUnsupportedUniversalTransitionRule.AssignMessage( conv(array[NUM_TVP_TLG_UNSUPPORTED_UNIVERSAL_TRANSITION_RULE].get<std::string>()).c_str() );
	TVPUnsupportedColorCount.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_COLOR_COUNT].get<std::string>()).c_str() );
	TVPDataFlagMustBeZero.AssignMessage( conv(array[NUM_TVP_DATA_FLAG_MUST_BE_ZERO].get<std::string>()).c_str() );
	TVPUnsupportedColorTypeColon.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_COLOR_TYPE_COLON].get<std::string>()).c_str() );
	TVPUnsupportedExternalGolombBitLengthTable.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_EXTERNAL_GOLOMB_BIT_LENGTH_TABLE].get<std::string>()).c_str() );
	TVPUnsupportedEntropyCodingMethod.AssignMessage( conv(array[NUM_TVP_UNSUPPORTED_ENTROPY_CODING_METHOD].get<std::string>()).c_str() );
	TVPInvalidTlgHeaderOrVersion.AssignMessage( conv(array[NUM_TVP_INVALID_TLG_HEADER_OR_VERSION].get<std::string>()).c_str() );
	TVPTlgMalformedTagMissionColonAfterNameLength.AssignMessage( conv(array[NUM_TVP_TLG_MALFORMED_TAG_MISSION_COLON_AFTER_NAME_LENGTH].get<std::string>()).c_str() );
	TVPTlgMalformedTagMissionEqualsAfterName.AssignMessage( conv(array[NUM_TVP_TLG_MALFORMED_TAG_MISSION_EQUALS_AFTER_NAME].get<std::string>()).c_str() );
	TVPTlgMalformedTagMissionColonAfterVaueLength.AssignMessage( conv(array[NUM_TVP_TLG_MALFORMED_TAG_MISSION_COLON_AFTER_VAUE_LENGTH].get<std::string>()).c_str() );
	TVPTlgMalformedTagMissionCommaAfterTag.AssignMessage( conv(array[NUM_TVP_TLG_MALFORMED_TAG_MISSION_COMMA_AFTER_TAG].get<std::string>()).c_str() );
	TVPFileSizeIsZero.AssignMessage( conv(array[NUM_TVP_FILE_SIZE_IS_ZERO].get<std::string>()).c_str() );
	TVPMemoryAllocationError.AssignMessage( conv(array[NUM_TVP_MEMORY_ALLOCATION_ERROR].get<std::string>()).c_str() );
	TVPFileReadError.AssignMessage( conv(array[NUM_TVP_FILE_READ_ERROR].get<std::string>()).c_str() );
	TVPInvalidPrerenderedFontFile.AssignMessage( conv(array[NUM_TVP_INVALID_PRERENDERED_FONT_FILE].get<std::string>()).c_str() );
	TVPNot16BitUnicodeFontFile.AssignMessage( conv(array[NUM_TVP_NOT16BIT_UNICODE_FONT_FILE].get<std::string>()).c_str() );
	TVPInvalidHeaderVersion.AssignMessage( conv(array[NUM_TVP_INVALID_HEADER_VERSION].get<std::string>()).c_str() );
	TVPTlgInsufficientMemory.AssignMessage( conv(array[NUM_TVP_TLG_INSUFFICIENT_MEMORY].get<std::string>()).c_str() );
	TVPTlgTooLargeBitLength.AssignMessage( conv(array[NUM_TVP_TLG_TOO_LARGE_BIT_LENGTH].get<std::string>()).c_str() );
	TVPCannotRetriveInterfaceFromDrawDevice.AssignMessage( conv(array[NUM_TVP_CANNOT_RETRIVE_INTERFACE_FROM_DRAW_DEVICE].get<std::string>()).c_str() );
	TVPIllegalCharacterConversionUTF16toUTF8.AssignMessage( conv(array[NUM_TVP_ILLEGAL_CHARACTER_CONVERSION_UTF16TO_UTF8].get<std::string>()).c_str() );
	TVPRequireLayerTreeOwnerInterfaceInterface.AssignMessage( conv(array[NUM_TVP_REQUIRE_LAYER_TREE_OWNER_INTERFACE_INTERFACE].get<std::string>()).c_str() );
	TVPRequireSlashEndOfDirectory.AssignMessage( conv(array[NUM_TVP_REQUIRE_SLASH_END_OF_DIRECTORY].get<std::string>()).c_str() );
	TVPScriptExceptionRaised.AssignMessage( conv(array[NUM_TVP_SCRIPT_EXCEPTION_RAISED].get<std::string>()).c_str() );
	TVPHardwareExceptionRaised.AssignMessage( conv(array[NUM_TVP_HARDWARE_EXCEPTION_RAISED].get<std::string>()).c_str() );
	TVPMainCDPName.AssignMessage( conv(array[NUM_TVP_MAIN_CDPNAME].get<std::string>()).c_str() );
	TVPExceptionCDPName.AssignMessage( conv(array[NUM_TVP_EXCEPTION_CDPNAME].get<std::string>()).c_str() );
	TVPCannnotLocateUIDLLForFolderSelection.AssignMessage( conv(array[NUM_TVP_CANNNOT_LOCATE_UIDLLFOR_FOLDER_SELECTION].get<std::string>()).c_str() );
	TVPInvalidUIDLL.AssignMessage( conv(array[NUM_TVP_INVALID_UIDLL].get<std::string>()).c_str() );
	TVPInvalidBPP.AssignMessage( conv(array[NUM_TVP_INVALID_BPP].get<std::string>()).c_str() );
	TVPCannotLoadPlugin.AssignMessage( conv(array[NUM_TVP_CANNOT_LOAD_PLUGIN].get<std::string>()).c_str() );
	TVPNotValidPlugin.AssignMessage( conv(array[NUM_TVP_NOT_VALID_PLUGIN].get<std::string>()).c_str() );
	TVPPluginUninitFailed.AssignMessage( conv(array[NUM_TVP_PLUGIN_UNINIT_FAILED].get<std::string>()).c_str() );
	TVPCannnotLinkPluginWhilePluginLinking.AssignMessage( conv(array[NUM_TVP_CANNNOT_LINK_PLUGIN_WHILE_PLUGIN_LINKING].get<std::string>()).c_str() );
	TVPNotSusiePlugin.AssignMessage( conv(array[NUM_TVP_NOT_SUSIE_PLUGIN].get<std::string>()).c_str() );
	TVPSusiePluginError.AssignMessage( conv(array[NUM_TVP_SUSIE_PLUGIN_ERROR].get<std::string>()).c_str() );
	TVPCannotReleasePlugin.AssignMessage( conv(array[NUM_TVP_CANNOT_RELEASE_PLUGIN].get<std::string>()).c_str() );
	TVPNotLoadedPlugin.AssignMessage( conv(array[NUM_TVP_NOT_LOADED_PLUGIN].get<std::string>()).c_str() );
	TVPCannotAllocateBitmapBits.AssignMessage( conv(array[NUM_TVP_CANNOT_ALLOCATE_BITMAP_BITS].get<std::string>()).c_str() );
	TVPScanLineRangeOver.AssignMessage( conv(array[NUM_TVP_SCAN_LINE_RANGE_OVER].get<std::string>()).c_str() );
	TVPPluginError.AssignMessage( conv(array[NUM_TVP_PLUGIN_ERROR].get<std::string>()).c_str() );
	TVPInvalidCDDADrive.AssignMessage( conv(array[NUM_TVP_INVALID_CDDADRIVE].get<std::string>()).c_str() );
	TVPCDDADriveNotFound.AssignMessage( conv(array[NUM_TVP_CDDADRIVE_NOT_FOUND].get<std::string>()).c_str() );
	TVPMCIError.AssignMessage( conv(array[NUM_TVP_MCIERROR].get<std::string>()).c_str() );
	TVPInvalidSMF.AssignMessage( conv(array[NUM_TVP_INVALID_SMF].get<std::string>()).c_str() );
	TVPMalformedMIDIMessage.AssignMessage( conv(array[NUM_TVP_MALFORMED_MIDIMESSAGE].get<std::string>()).c_str() );
	TVPCannotInitDirectSound.AssignMessage( conv(array[NUM_TVP_CANNOT_INIT_DIRECT_SOUND].get<std::string>()).c_str() );
	TVPCannotCreateDSSecondaryBuffer.AssignMessage( conv(array[NUM_TVP_CANNOT_CREATE_DSSECONDARY_BUFFER].get<std::string>()).c_str() );
	TVPInvalidLoopInformation.AssignMessage( conv(array[NUM_TVP_INVALID_LOOP_INFORMATION].get<std::string>()).c_str() );
	TVPNotChildMenuItem.AssignMessage( conv(array[NUM_TVP_NOT_CHILD_MENU_ITEM].get<std::string>()).c_str() );
	TVPCannotInitDirect3D.AssignMessage( conv(array[NUM_TVP_CANNOT_INIT_DIRECT3D].get<std::string>()).c_str() );
	TVPCannotFindDisplayMode.AssignMessage( conv(array[NUM_TVP_CANNOT_FIND_DISPLAY_MODE].get<std::string>()).c_str() );
	TVPCannotSwitchToFullScreen.AssignMessage( conv(array[NUM_TVP_CANNOT_SWITCH_TO_FULL_SCREEN].get<std::string>()).c_str() );
	TVPInvalidPropertyInFullScreen.AssignMessage( conv(array[NUM_TVP_INVALID_PROPERTY_IN_FULL_SCREEN].get<std::string>()).c_str() );
	TVPInvalidMethodInFullScreen.AssignMessage( conv(array[NUM_TVP_INVALID_METHOD_IN_FULL_SCREEN].get<std::string>()).c_str() );
	TVPCannotLoadCursor.AssignMessage( conv(array[NUM_TVP_CANNOT_LOAD_CURSOR].get<std::string>()).c_str() );
	TVPCannotLoadKrMovieDLL.AssignMessage( conv(array[NUM_TVP_CANNOT_LOAD_KR_MOVIE_DLL].get<std::string>()).c_str() );
	TVPInvalidKrMovieDLL.AssignMessage( conv(array[NUM_TVP_INVALID_KR_MOVIE_DLL].get<std::string>()).c_str() );
	TVPErrorInKrMovieDLL.AssignMessage( conv(array[NUM_TVP_ERROR_IN_KR_MOVIE_DLL].get<std::string>()).c_str() );
	TVPWindowAlreadyMissing.AssignMessage( conv(array[NUM_TVP_WINDOW_ALREADY_MISSING].get<std::string>()).c_str() );
	TVPPrerenderedFontMappingFailed.AssignMessage( conv(array[NUM_TVP_PRERENDERED_FONT_MAPPING_FAILED].get<std::string>()).c_str() );
	TVPConfigFailOriginalFileCannotBeRewritten.AssignMessage( conv(array[NUM_TVP_CONFIG_FAIL_ORIGINAL_FILE_CANNOT_BE_REWRITTEN].get<std::string>()).c_str() );
	TVPConfigFailTempExeNotErased.AssignMessage( conv(array[NUM_TVP_CONFIG_FAIL_TEMP_EXE_NOT_ERASED].get<std::string>()).c_str() );
	TVPExecutionFail.AssignMessage( conv(array[NUM_TVP_EXECUTION_FAIL].get<std::string>()).c_str() );
	TVPPluginUnboundFunctionError.AssignMessage( conv(array[NUM_TVP_PLUGIN_UNBOUND_FUNCTION_ERROR].get<std::string>()).c_str() );
	TVPExceptionHadBeenOccured.AssignMessage( conv(array[NUM_TVP_EXCEPTION_HAD_BEEN_OCCURED].get<std::string>()).c_str() );
	TVPConsoleResult.AssignMessage( conv(array[NUM_TVP_CONSOLE_RESULT].get<std::string>()).c_str() );
	TVPInfoListingFiles.AssignMessage( conv(array[NUM_TVP_INFO_LISTING_FILES].get<std::string>()).c_str() );
	TVPInfoTotalPhysicalMemory.AssignMessage( conv(array[NUM_TVP_INFO_TOTAL_PHYSICAL_MEMORY].get<std::string>()).c_str() );
	TVPInfoSelectedProjectDirectory.AssignMessage( conv(array[NUM_TVP_INFO_SELECTED_PROJECT_DIRECTORY].get<std::string>()).c_str() );
	TVPTooSmallExecutableSize.AssignMessage( conv(array[NUM_TVP_TOO_SMALL_EXECUTABLE_SIZE].get<std::string>()).c_str() );
	TVPInfoLoadingExecutableEmbeddedOptionsFailed.AssignMessage( conv(array[NUM_TVP_INFO_LOADING_EXECUTABLE_EMBEDDED_OPTIONS_FAILED].get<std::string>()).c_str() );
	TVPInfoLoadingExecutableEmbeddedOptionsSucceeded.AssignMessage( conv(array[NUM_TVP_INFO_LOADING_EXECUTABLE_EMBEDDED_OPTIONS_SUCCEEDED].get<std::string>()).c_str() );
	TVPFileNotFound.AssignMessage( conv(array[NUM_TVP_FILE_NOT_FOUND].get<std::string>()).c_str() );
	TVPInfoLoadingConfigurationFileFailed.AssignMessage( conv(array[NUM_TVP_INFO_LOADING_CONFIGURATION_FILE_FAILED].get<std::string>()).c_str() );
	TVPInfoLoadingConfigurationFileSucceeded.AssignMessage( conv(array[NUM_TVP_INFO_LOADING_CONFIGURATION_FILE_SUCCEEDED].get<std::string>()).c_str() );
	TVPInfoDataPathDoesNotExistTryingToMakeIt.AssignMessage( conv(array[NUM_TVP_INFO_DATA_PATH_DOES_NOT_EXIST_TRYING_TO_MAKE_IT].get<std::string>()).c_str() );
	TVPOk.AssignMessage( conv(array[NUM_TVP_OK].get<std::string>()).c_str() );
	TVPFaild.AssignMessage( conv(array[NUM_TVP_FAILD].get<std::string>()).c_str() );
	TVPInfoDataPath.AssignMessage( conv(array[NUM_TVP_INFO_DATA_PATH].get<std::string>()).c_str() );
	TVPInfoSpecifiedOptionEarlierItemHasMorePriority.AssignMessage( conv(array[NUM_TVP_INFO_SPECIFIED_OPTION_EARLIER_ITEM_HAS_MORE_PRIORITY].get<std::string>()).c_str() );
	TVPNone.AssignMessage( conv(array[NUM_TVP_NONE].get<std::string>()).c_str() );
	TVPInfoCpuClockRoughly.AssignMessage( conv(array[NUM_TVP_INFO_CPU_CLOCK_ROUGHLY].get<std::string>()).c_str() );
	TVPProgramStartedOn.AssignMessage( conv(array[NUM_TVP_PROGRAM_STARTED_ON].get<std::string>()).c_str() );
	TVPKirikiri.AssignMessage( conv(array[NUM_TVP_KIRIKIRI].get<std::string>()).c_str() );
	TVPUnknownError.AssignMessage( conv(array[NUM_TVP_UNKNOWN_ERROR].get<std::string>()).c_str() );
	TVPExitCode.AssignMessage( conv(array[NUM_TVP_EXIT_CODE].get<std::string>()).c_str() );
	TVPFatalError.AssignMessage( conv(array[NUM_TVP_FATAL_ERROR].get<std::string>()).c_str() );
	TVPEnableDigitizer.AssignMessage( conv(array[NUM_TVP_ENABLE_DIGITIZER].get<std::string>()).c_str() );
	TVPTouchIntegratedTouch.AssignMessage( conv(array[NUM_TVP_TOUCH_INTEGRATED_TOUCH].get<std::string>()).c_str() );
	TVPTouchExternalTouch.AssignMessage( conv(array[NUM_TVP_TOUCH_EXTERNAL_TOUCH].get<std::string>()).c_str() );
	TVPTouchIntegratedPen.AssignMessage( conv(array[NUM_TVP_TOUCH_INTEGRATED_PEN].get<std::string>()).c_str() );
	TVPTouchExternalPen.AssignMessage( conv(array[NUM_TVP_TOUCH_EXTERNAL_PEN].get<std::string>()).c_str() );
	TVPTouchMultiInput.AssignMessage( conv(array[NUM_TVP_TOUCH_MULTI_INPUT].get<std::string>()).c_str() );
	TVPTouchReady.AssignMessage( conv(array[NUM_TVP_TOUCH_READY].get<std::string>()).c_str() );
	TVPCpuCheckFailure.AssignMessage( conv(array[NUM_TVP_CPU_CHECK_FAILURE].get<std::string>()).c_str() );
	TVPCpuCheckFailureCpuFamilyOrLesserIsNotSupported.AssignMessage( conv(array[NUM_TVP_CPU_CHECK_FAILURE_CPU_FAMILY_OR_LESSER_IS_NOT_SUPPORTED].get<std::string>()).c_str() );
	TVPInfoCpuNumber.AssignMessage( conv(array[NUM_TVP_INFO_CPU_NUMBER].get<std::string>()).c_str() );
	TVPCpuCheckFailureNotSupprtedCpu.AssignMessage( conv(array[NUM_TVP_CPU_CHECK_FAILURE_NOT_SUPPRTED_CPU].get<std::string>()).c_str() );
	TVPInfoFinallyDetectedCpuFeatures.AssignMessage( conv(array[NUM_TVP_INFO_FINALLY_DETECTED_CPU_FEATURES].get<std::string>()).c_str() );
	TVPCpuCheckFailureNotSupportedCpu.AssignMessage( conv(array[NUM_TVP_CPU_CHECK_FAILURE_NOT_SUPPORTED_CPU].get<std::string>()).c_str() );
	TVPInfoCpuClock.AssignMessage( conv(array[NUM_TVP_INFO_CPU_CLOCK].get<std::string>()).c_str() );
	TVPLayerBitmapBufferUnderrunDetectedCheckYourDrawingCode.AssignMessage( conv(array[NUM_TVP_LAYER_BITMAP_BUFFER_UNDERRUN_DETECTED_CHECK_YOUR_DRAWING_CODE].get<std::string>()).c_str() );
	TVPLayerBitmapBufferOverrunDetectedCheckYourDrawingCode.AssignMessage( conv(array[NUM_TVP_LAYER_BITMAP_BUFFER_OVERRUN_DETECTED_CHECK_YOUR_DRAWING_CODE].get<std::string>()).c_str() );
	TVPFaildToCreateDirect3D.AssignMessage( conv(array[NUM_TVP_FAILD_TO_CREATE_DIRECT3D].get<std::string>()).c_str() );
	TVPFaildToDecideBackbufferFormat.AssignMessage( conv(array[NUM_TVP_FAILD_TO_DECIDE_BACKBUFFER_FORMAT].get<std::string>()).c_str() );
	TVPFaildToCreateDirect3DDevice.AssignMessage( conv(array[NUM_TVP_FAILD_TO_CREATE_DIRECT3DDEVICE].get<std::string>()).c_str() );
	TVPFaildToSetViewport.AssignMessage( conv(array[NUM_TVP_FAILD_TO_SET_VIEWPORT].get<std::string>()).c_str() );
	TVPFaildToSetRenderState.AssignMessage( conv(array[NUM_TVP_FAILD_TO_SET_RENDER_STATE].get<std::string>()).c_str() );
	TVPWarningImageSizeTooLargeMayBeCannotCreateTexture.AssignMessage( conv(array[NUM_TVP_WARNING_IMAGE_SIZE_TOO_LARGE_MAY_BE_CANNOT_CREATE_TEXTURE].get<std::string>()).c_str() );
	TVPUsePowerOfTwoSurface.AssignMessage( conv(array[NUM_TVP_USE_POWER_OF_TWO_SURFACE].get<std::string>()).c_str() );
	TVPCannotAllocateD3DOffScreenSurface.AssignMessage( conv(array[NUM_TVP_CANNOT_ALLOCATE_D3DOFF_SCREEN_SURFACE].get<std::string>()).c_str() );
	TVPBasicDrawDeviceFailedToCreateDirect3DDevices.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICES].get<std::string>()).c_str() );
	TVPBasicDrawDeviceFailedToCreateDirect3DDevicesUnknownReason.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICES_UNKNOWN_REASON].get<std::string>()).c_str() );
	TVPBasicDrawDeviceTextureHasAlreadyBeenLocked.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_TEXTURE_HAS_ALREADY_BEEN_LOCKED].get<std::string>()).c_str() );
	TVPInternalErrorResult.AssignMessage( conv(array[NUM_TVP_INTERNAL_ERROR_RESULT].get<std::string>()).c_str() );
	TVPBasicDrawDeviceInfPolygonDrawingFailed.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_INF_POLYGON_DRAWING_FAILED].get<std::string>()).c_str() );
	TVPBasicDrawDeviceInfDirect3DDevicePresentFailed.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_INF_DIRECT3DDEVICE_PRESENT_FAILED].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeRestart.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_RESTART].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeBadFlags.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_FLAGS].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeBadParam.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_PARAM].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeFailed.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_FAILED].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeBadMode.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_BAD_MODE].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedDispChangeNotUpdated.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_DISP_CHANGE_NOT_UPDATED].get<std::string>()).c_str() );
	TVPChangeDisplaySettingsFailedUnknownReason.AssignMessage( conv(array[NUM_TVP_CHANGE_DISPLAY_SETTINGS_FAILED_UNKNOWN_REASON].get<std::string>()).c_str() );
	TVPFailedToCreateScreenDC.AssignMessage( conv(array[NUM_TVP_FAILED_TO_CREATE_SCREEN_DC].get<std::string>()).c_str() );
	TVPFailedToCreateOffscreenBitmap.AssignMessage( conv(array[NUM_TVP_FAILED_TO_CREATE_OFFSCREEN_BITMAP].get<std::string>()).c_str() );
	TVPFailedToCreateOffscreenDC.AssignMessage( conv(array[NUM_TVP_FAILED_TO_CREATE_OFFSCREEN_DC].get<std::string>()).c_str() );
	TVPInfoSusiePluginInfo.AssignMessage( conv(array[NUM_TVP_INFO_SUSIE_PLUGIN_INFO].get<std::string>()).c_str() );
	TVPSusiePluginUnsupportedBitmapHeader.AssignMessage( conv(array[NUM_TVP_SUSIE_PLUGIN_UNSUPPORTED_BITMAP_HEADER].get<std::string>()).c_str() );
	TVPBasicDrawDeviceFailedToCreateDirect3DDevice.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICE].get<std::string>()).c_str() );
	TVPBasicDrawDeviceFailedToCreateDirect3DDeviceUnknownReason.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_FAILED_TO_CREATE_DIRECT3DDEVICE_UNKNOWN_REASON].get<std::string>()).c_str() );
	TVPCouldNotCreateAnyDrawDevice.AssignMessage( conv(array[NUM_TVP_COULD_NOT_CREATE_ANY_DRAW_DEVICE].get<std::string>()).c_str() );
	TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne.AssignMessage( conv(array[NUM_TVP_BASIC_DRAW_DEVICE_DOES_NOT_SUPPORTE_LAYER_MANAGER_MORE_THAN_ONE].get<std::string>()).c_str() );
	TVPInvalidVideoSize.AssignMessage( conv(array[NUM_TVP_INVALID_VIDEO_SIZE].get<std::string>()).c_str() );
	TVPRoughVsyncIntervalReadFromApi.AssignMessage( conv(array[NUM_TVP_ROUGH_VSYNC_INTERVAL_READ_FROM_API].get<std::string>()).c_str() );
	TVPRoughVsyncIntervalStillSeemsWrong.AssignMessage( conv(array[NUM_TVP_ROUGH_VSYNC_INTERVAL_STILL_SEEMS_WRONG].get<std::string>()).c_str() );
	TVPInfoFoundDirect3DInterface.AssignMessage( conv(array[NUM_TVP_INFO_FOUND_DIRECT3DINTERFACE].get<std::string>()).c_str() );
	TVPInfoFaild.AssignMessage( conv(array[NUM_TVP_INFO_FAILD].get<std::string>()).c_str() );
	TVPInfoDirect3D.AssignMessage( conv(array[NUM_TVP_INFO_DIRECT3D].get<std::string>()).c_str() );
	TVPCannotLoadD3DDLL.AssignMessage( conv(array[NUM_TVP_CANNOT_LOAD_D3DDLL].get<std::string>()).c_str() );
	TVPNotFoundDirect3DCreate.AssignMessage( conv(array[NUM_TVP_NOT_FOUND_DIRECT3DCREATE].get<std::string>()).c_str() );
	TVPInfoEnvironmentUsing.AssignMessage( conv(array[NUM_TVP_INFO_ENVIRONMENT_USING].get<std::string>()).c_str() );
	TVPInfoSearchBestFullscreenResolution.AssignMessage( conv(array[NUM_TVP_INFO_SEARCH_BEST_FULLSCREEN_RESOLUTION].get<std::string>()).c_str() );
	TVPInfoConditionPreferredScreenMode.AssignMessage( conv(array[NUM_TVP_INFO_CONDITION_PREFERRED_SCREEN_MODE].get<std::string>()).c_str() );
	TVPInfoConditionMode.AssignMessage( conv(array[NUM_TVP_INFO_CONDITION_MODE].get<std::string>()).c_str() );
	TVPInfoConditionZoomMode.AssignMessage( conv(array[NUM_TVP_INFO_CONDITION_ZOOM_MODE].get<std::string>()).c_str() );
	TVPInfoEnvironmentDefaultScreenMode.AssignMessage( conv(array[NUM_TVP_INFO_ENVIRONMENT_DEFAULT_SCREEN_MODE].get<std::string>()).c_str() );
	TVPInfoEnvironmentDefaultScreenAspectRatio.AssignMessage( conv(array[NUM_TVP_INFO_ENVIRONMENT_DEFAULT_SCREEN_ASPECT_RATIO].get<std::string>()).c_str() );
	TVPInfoEnvironmentAvailableDisplayModes.AssignMessage( conv(array[NUM_TVP_INFO_ENVIRONMENT_AVAILABLE_DISPLAY_MODES].get<std::string>()).c_str() );
	TVPInfoNotFoundScreenModeFromDriver.AssignMessage( conv(array[NUM_TVP_INFO_NOT_FOUND_SCREEN_MODE_FROM_DRIVER].get<std::string>()).c_str() );
	TVPInfoResultCandidates.AssignMessage( conv(array[NUM_TVP_INFO_RESULT_CANDIDATES].get<std::string>()).c_str() );
	TVPInfoTryScreenMode.AssignMessage( conv(array[NUM_TVP_INFO_TRY_SCREEN_MODE].get<std::string>()).c_str() );
	TVPAllScreenModeError.AssignMessage( conv(array[NUM_TVP_ALL_SCREEN_MODE_ERROR].get<std::string>()).c_str() );
	TVPInfoChangeScreenModeSuccess.AssignMessage( conv(array[NUM_TVP_INFO_CHANGE_SCREEN_MODE_SUCCESS].get<std::string>()).c_str() );
	TVPSelectXP3FileOrFolder.AssignMessage( conv(array[NUM_TVP_SELECT_XP3FILE_OR_FOLDER].get<std::string>()).c_str() );
	TVPD3dErrDeviceLost.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DEVICE_LOST].get<std::string>()).c_str() );
	TVPD3dErrDriverIinternalError.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DRIVER_IINTERNAL_ERROR].get<std::string>()).c_str() );
	TVPD3dErrInvalidCall.AssignMessage( conv(array[NUM_TVP_D3D_ERR_INVALID_CALL].get<std::string>()).c_str() );
	TVPD3dErrOutOfVideoMemory.AssignMessage( conv(array[NUM_TVP_D3D_ERR_OUT_OF_VIDEO_MEMORY].get<std::string>()).c_str() );
	TVPD3dErrOutOfMemory.AssignMessage( conv(array[NUM_TVP_D3D_ERR_OUT_OF_MEMORY].get<std::string>()).c_str() );
	TVPD3dErrWrongTextureFormat.AssignMessage( conv(array[NUM_TVP_D3D_ERR_WRONG_TEXTURE_FORMAT].get<std::string>()).c_str() );
	TVPD3dErrUnsuportedColorOperation.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPORTED_COLOR_OPERATION].get<std::string>()).c_str() );
	TVPD3dErrUnsuportedColorArg.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPORTED_COLOR_ARG].get<std::string>()).c_str() );
	TVPD3dErrUnsuportedAalphtOperation.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPORTED_AALPHT_OPERATION].get<std::string>()).c_str() );
	TVPD3dErrUnsuportedAlphaArg.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPORTED_ALPHA_ARG].get<std::string>()).c_str() );
	TVPD3dErrTooManyOperations.AssignMessage( conv(array[NUM_TVP_D3D_ERR_TOO_MANY_OPERATIONS].get<std::string>()).c_str() );
	TVPD3dErrConflictioningTextureFilter.AssignMessage( conv(array[NUM_TVP_D3D_ERR_CONFLICTIONING_TEXTURE_FILTER].get<std::string>()).c_str() );
	TVPD3dErrUnsuportedFactorValue.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPORTED_FACTOR_VALUE].get<std::string>()).c_str() );
	TVPD3dErrConflictioningRenderState.AssignMessage( conv(array[NUM_TVP_D3D_ERR_CONFLICTIONING_RENDER_STATE].get<std::string>()).c_str() );
	TVPD3dErrUnsupportedTextureFilter.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPPORTED_TEXTURE_FILTER].get<std::string>()).c_str() );
	TVPD3dErrConflictioningTexturePalette.AssignMessage( conv(array[NUM_TVP_D3D_ERR_CONFLICTIONING_TEXTURE_PALETTE].get<std::string>()).c_str() );
	TVPD3dErrNotFound.AssignMessage( conv(array[NUM_TVP_D3D_ERR_NOT_FOUND].get<std::string>()).c_str() );
	TVPD3dErrMoreData.AssignMessage( conv(array[NUM_TVP_D3D_ERR_MORE_DATA].get<std::string>()).c_str() );
	TVPD3dErrDeviceNotReset.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DEVICE_NOT_RESET].get<std::string>()).c_str() );
	TVPD3dErrNotAvailable.AssignMessage( conv(array[NUM_TVP_D3D_ERR_NOT_AVAILABLE].get<std::string>()).c_str() );
	TVPD3dErrInvalidDevice.AssignMessage( conv(array[NUM_TVP_D3D_ERR_INVALID_DEVICE].get<std::string>()).c_str() );
	TVPD3dErrDriverInvalidCall.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DRIVER_INVALID_CALL].get<std::string>()).c_str() );
	TVPD3dErrWasStillDrawing.AssignMessage( conv(array[NUM_TVP_D3D_ERR_WAS_STILL_DRAWING].get<std::string>()).c_str() );
	TVPD3dErrDeviceHung.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DEVICE_HUNG].get<std::string>()).c_str() );
	TVPD3dErrUnsupportedOverlay.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPPORTED_OVERLAY].get<std::string>()).c_str() );
	TVPD3dErrUnsupportedOverlayFormat.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPPORTED_OVERLAY_FORMAT].get<std::string>()).c_str() );
	TVPD3dErrCannotProtectContent.AssignMessage( conv(array[NUM_TVP_D3D_ERR_CANNOT_PROTECT_CONTENT].get<std::string>()).c_str() );
	TVPD3dErrUnsupportedCrypto.AssignMessage( conv(array[NUM_TVP_D3D_ERR_UNSUPPORTED_CRYPTO].get<std::string>()).c_str() );
	TVPD3dErrPresentStatisticsDisJoint.AssignMessage( conv(array[NUM_TVP_D3D_ERR_PRESENT_STATISTICS_DIS_JOINT].get<std::string>()).c_str() );
	TVPD3dErrDeviceRemoved.AssignMessage( conv(array[NUM_TVP_D3D_ERR_DEVICE_REMOVED].get<std::string>()).c_str() );
	TVPD3dOkNoAutoGen.AssignMessage( conv(array[NUM_TVP_D3D_OK_NO_AUTO_GEN].get<std::string>()).c_str() );
	TVPD3dErrFail.AssignMessage( conv(array[NUM_TVP_D3D_ERR_FAIL].get<std::string>()).c_str() );
	TVPD3dErrInvalidArg.AssignMessage( conv(array[NUM_TVP_D3D_ERR_INVALID_ARG].get<std::string>()).c_str() );
	TVPD3dUnknownError.AssignMessage( conv(array[NUM_TVP_D3D_UNKNOWN_ERROR].get<std::string>()).c_str() );
	TVPExceptionAccessViolation.AssignMessage( conv(array[NUM_TVP_EXCEPTION_ACCESS_VIOLATION].get<std::string>()).c_str() );
	TVPExceptionBreakpoint.AssignMessage( conv(array[NUM_TVP_EXCEPTION_BREAKPOINT].get<std::string>()).c_str() );
	TVPExceptionDatatypeMisalignment.AssignMessage( conv(array[NUM_TVP_EXCEPTION_DATATYPE_MISALIGNMENT].get<std::string>()).c_str() );
	TVPExceptionSingleStep.AssignMessage( conv(array[NUM_TVP_EXCEPTION_SINGLE_STEP].get<std::string>()).c_str() );
	TVPExceptionArrayBoundsExceeded.AssignMessage( conv(array[NUM_TVP_EXCEPTION_ARRAY_BOUNDS_EXCEEDED].get<std::string>()).c_str() );
	TVPExceptionFltDenormalOperand.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_DENORMAL_OPERAND].get<std::string>()).c_str() );
	TVPExceptionFltDivideByZero.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_DIVIDE_BY_ZERO].get<std::string>()).c_str() );
	TVPExceptionFltInexactResult.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_INEXACT_RESULT].get<std::string>()).c_str() );
	TVPExceptionFltInvalidOperation.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_INVALID_OPERATION].get<std::string>()).c_str() );
	TVPExceptionFltOverflow.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_OVERFLOW].get<std::string>()).c_str() );
	TVPExceptionFltStackCheck.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_STACK_CHECK].get<std::string>()).c_str() );
	TVPExceptionFltUnderflow.AssignMessage( conv(array[NUM_TVP_EXCEPTION_FLT_UNDERFLOW].get<std::string>()).c_str() );
	TVPExceptionIntDivideByZero.AssignMessage( conv(array[NUM_TVP_EXCEPTION_INT_DIVIDE_BY_ZERO].get<std::string>()).c_str() );
	TVPExceptionIntOverflow.AssignMessage( conv(array[NUM_TVP_EXCEPTION_INT_OVERFLOW].get<std::string>()).c_str() );
	TVPExceptionPrivInstruction.AssignMessage( conv(array[NUM_TVP_EXCEPTION_PRIV_INSTRUCTION].get<std::string>()).c_str() );
	TVPExceptionNoncontinuableException.AssignMessage( conv(array[NUM_TVP_EXCEPTION_NONCONTINUABLE_EXCEPTION].get<std::string>()).c_str() );
	TVPExceptionGuardPage.AssignMessage( conv(array[NUM_TVP_EXCEPTION_GUARD_PAGE].get<std::string>()).c_str() );
	TVPExceptionIllegalInstruction.AssignMessage( conv(array[NUM_TVP_EXCEPTION_ILLEGAL_INSTRUCTION].get<std::string>()).c_str() );
	TVPExceptionInPageError.AssignMessage( conv(array[NUM_TVP_EXCEPTION_IN_PAGE_ERROR].get<std::string>()).c_str() );
	TVPExceptionInvalidDisposition.AssignMessage( conv(array[NUM_TVP_EXCEPTION_INVALID_DISPOSITION].get<std::string>()).c_str() );
	TVPExceptionInvalidHandle.AssignMessage( conv(array[NUM_TVP_EXCEPTION_INVALID_HANDLE].get<std::string>()).c_str() );
	TVPExceptionStackOverflow.AssignMessage( conv(array[NUM_TVP_EXCEPTION_STACK_OVERFLOW].get<std::string>()).c_str() );
	TVPExceptionUnwindCconsolidate.AssignMessage( conv(array[NUM_TVP_EXCEPTION_UNWIND_CCONSOLIDATE].get<std::string>()).c_str() );
	TVPCannotShowModalAreadyShowed.AssignMessage( conv(array[NUM_TVP_CANNOT_SHOW_MODAL_AREADY_SHOWED].get<std::string>()).c_str() );
	TVPCannotShowModalSingleWindow.AssignMessage( conv(array[NUM_TVP_CANNOT_SHOW_MODAL_SINGLE_WINDOW].get<std::string>()).c_str() );

}
