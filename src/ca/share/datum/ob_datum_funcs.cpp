namespace oceanbase {
namespace common {
namespace ca {

template <ObCollationType CS_TYPE, bool WITH_END_SPACE, bool NULL_FIRST>
struct ObTeeNullSafeDatumStrCmp
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret) {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(l.is_null()) && OB_UNLIKELY(r.is_null())) {
      cmp_ret = 0;
    } else if (OB_UNLIKELY(l.is_null())) {
      cmp_ret = NULL_FIRST ? -1 : 1;
    } else if (OB_UNLIKELY(r.is_null())) {
      cmp_ret = NULL_FIRST ? 1 : -1;
    } else {
      int64_t l_pos = 0;
      int64_t l_outlen = ObBase64Encoder::needed_decoded_length(l.pack_);
      // csch demo 为了实现简单，分配内存时没有使用 OB 自带的内存分配器（各种 allocator）
      //      实际开发时，需要使用相应模块的 allocator 作为内存分配器
      //      以便控制租户内存使用量、各模块内存使用量
      char *l_output = (char *)malloc(l_outlen + 1);
      memset(l_output, 0, l_outlen);
      if (OB_FAIL(ObBase64Encoder::decode(l.ptr_, l.pack_,
                                          reinterpret_cast<uint8_t*>(l_output),
                                          l_outlen, l_pos))) {
         if (OB_UNLIKELY(ret == OB_INVALID_ARGUMENT)) {
           memcpy(l_output, l.ptr_, l.pack_);
         } else {
          cmp_ret = NULL_FIRST ? -1 : 1;
         }
      }

      int64_t r_pos = 0;
      int64_t r_outlen = ObBase64Encoder::needed_decoded_length(r.pack_);
      char *r_output = (char *)malloc(r_outlen + 1);
      memset(r_output, 0, r_outlen);
      if (OB_FAIL(ObBase64Encoder::decode(r.ptr_, r.pack_,
                  reinterpret_cast<uint8_t*>(r_output),
                  r_outlen, r_pos))) {
          if (OB_UNLIKELY(ret = OB_INVALID_ARGUMENT)) {
             memcpy(r_output, r.ptr_, r.pack_);
          } else {
            cmp_ret = NULL_FIRST ? 1 : -1;
          }
      }

      ObDatum l_decode;
      l_decode.set_string(l_output, (uint32_t)l_outlen);

      ObDatum r_decode;
      r_decode.set_string(r_output, (uint32_t)r_outlen);

      ret = datum_cmp::ObDatumStrCmp<CS_TYPE, WITH_END_SPACE>::cmp(l_decode, r_decode, cmp_ret);
    }
    return ret;
  }
};

} // end namespace ca
} // end namespace common
} // end namespace oceanbase