/* The resident software DC's initial font is fixed at 8x8 pixels. */
#define GDI_TEXT_CELL 8
#define GDI_TEXT_LIMIT 256
#define GDI_ETO_OPAQUE 2u
#define GDI_ETO_CLIPPED 4u
#define GDI_GGTE_WIN3_EXTENT 1u

static VOID text_cell(const ARM64_GDI_RASTER *raster, const ARM64_USER_RECT *clip,
                      ARM64_USER_RECT *dirty, LONGLONG x, LONGLONG y,
                      LONGLONG width, LONGLONG height, const UCHAR *glyph)
{
    LONGLONG left = max(x, clip->Left), top = max(y, clip->Top);
    LONGLONG right = min(x + width, clip->Right), bottom = min(y + height, clip->Bottom);
    if (left >= right || top >= bottom) return;

    ARM64_USER_RECT bounds = {(LONG)left, (LONG)top, (LONG)right, (LONG)bottom};
    union_bounds(dirty, &bounds);
    for (LONG row = bounds.Top; row < bounds.Bottom; ++row) {
        UCHAR bits = glyph ? glyph[row - y] : 0;
        PULONG pixels = raster->Pixels + (SIZE_T)row * raster->Stride;
        for (LONG col = bounds.Left; col < bounds.Right; ++col) {
            if (raster->Clip && !RtlCheckBit(&raster->Clip->Bits, (ULONG)row * raster->Width + col))
                continue;
            pixels[col] = glyph && (bits & (1u << (col - x))) ? 0 : 0xffffffu;
        }
        if (raster->Display) flush_display(pixels + bounds.Left, (bounds.Right - bounds.Left) * sizeof(ULONG));
    }
}

static ULONG_PTR gdi_ext_text_out(WIN32_ARGS)
{
    ULONG flags = (ULONG)d;
    LONG count = (LONG)g;
    if ((flags & ~(GDI_ETO_OPAQUE | GDI_ETO_CLIPPED)) || count < 0 || count > GDI_TEXT_LIMIT)
        return FALSE;

    ARM64_USER_RECT rect;
    if (e && flags && ((e & 3) || !NT_SUCCESS(copy_user_input(e, &rect, sizeof(rect))))) return FALSE;
    if (!count && !(e && (flags & GDI_ETO_OPAQUE))) return TRUE;
    if (flags && !e) return FALSE;

    WCHAR chars[GDI_TEXT_LIMIT];
    LONG advance[GDI_TEXT_LIMIT];
    if (count && ((f & 1) || !NT_SUCCESS(copy_user_input(f, chars, count * sizeof(*chars))) ||
                  (h && ((h & 3) || !NT_SUCCESS(copy_user_input(h, advance, count * sizeof(*advance)))))))
        return FALSE;

    gdi_enter();
    ARM64_GDI_RASTER raster;
    BOOLEAN valid = gdi_dc_raster(a, TRUE, &raster);
    if (valid) {
        ARM64_USER_RECT clip = {0, 0, raster.Width, raster.Height}, dirty = {0};
        if (flags & GDI_ETO_OPAQUE)
            text_cell(&raster, &clip, &dirty, rect.Left, rect.Top,
                      (LONGLONG)rect.Right - rect.Left, (LONGLONG)rect.Bottom - rect.Top, NULL);
        if (flags & GDI_ETO_CLIPPED) {
            clip.Left = max(clip.Left, rect.Left);
            clip.Top = max(clip.Top, rect.Top);
            clip.Right = min(clip.Right, rect.Right);
            clip.Bottom = min(clip.Bottom, rect.Bottom);
        }
        LONGLONG x = (LONG)b;
        for (LONG index = 0; index < count; ++index) {
            WCHAR ch = chars[index];
            const UCHAR *glyph = font8x8_basic[ch < (sizeof(font8x8_basic) / sizeof(font8x8_basic[0])) ? ch : '?'];
            text_cell(&raster, &clip, &dirty, x, (LONG)c, GDI_TEXT_CELL, GDI_TEXT_CELL, glyph);
            x += h ? advance[index] : GDI_TEXT_CELL;
        }
        if (raster.Window && raster.Window->Visible && !empty_rect(&dirty))
            present_window(raster.Window, dirty.Left, dirty.Top,
                           dirty.Right - dirty.Left, dirty.Bottom - dirty.Top);
    }
    gdi_leave();
    return valid;
}

static ULONG_PTR gdi_get_text_extent(WIN32_ARGS)
{
    LONG count = (LONG)c;
    if (((ULONG)e & ~GDI_GGTE_WIN3_EXTENT) || count < 0 || count > GDI_TEXT_LIMIT || (d & 3)) return FALSE;
    if (count) {
        WCHAR chars[GDI_TEXT_LIMIT];
        if ((b & 1) || !NT_SUCCESS(copy_user_input(b, chars, count * sizeof(*chars)))) return FALSE;
        gdi_enter();
        ARM64_GDI_RASTER raster;
        BOOLEAN valid = gdi_dc_raster(a, TRUE, &raster);
        gdi_leave();
        if (!valid) return FALSE;
    }
    /* A fixed cell has zero overhang for both supported extent modes. */
    LONG size[2] = {count * GDI_TEXT_CELL, count ? GDI_TEXT_CELL : 0};
    return NT_SUCCESS(copy_user_buffer(d, size, sizeof(size), TRUE));
}
