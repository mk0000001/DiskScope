#include "volume_view.h"
#include "ui.h"

static char *size_text(DsSize value)
{
    switch (value.quality) {
    case DS_KNOWN: return g_format_size_full(value.bytes, G_FORMAT_SIZE_IEC_UNITS);
    case DS_UNSUPPORTED: return g_strdup(UI("지원되지 않음"));
    case DS_OVERFLOW: return g_strdup(UI("범위 초과"));
    case DS_CHANGED: return g_strdup(UI("일관되지 않은 값"));
    default: return g_strdup(UI("확인 못함"));
    }
}

char *ds_ui_volume_summary(DsStore *store, DsError *error)
{
    DsVolumeInfo volume;
    if (ds_store_volume(store, &volume, error) != DS_OK)
        return NULL;
    if (!volume.captured_at)
        return g_strdup(UI("이 결과에 저장된 볼륨 정보가 없습니다."));

    g_autofree char *total = size_text(volume.total);
    g_autofree char *used = size_text(volume.used);
    g_autofree char *available = size_text(volume.available);
    return g_strdup_printf(UI("스캔 시작 시점의 전체 볼륨\n용량  %s\nOS 보고 사용량  %s\n사용자 가용 공간  %s"),
                           total, used, available);
}
