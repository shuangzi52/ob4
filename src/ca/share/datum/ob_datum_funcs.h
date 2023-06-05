namespace oceanbase {
namespace common {
namespace ca {

template <ObCollationType CS_TYPE, bool WITH_END_SPACE, bool NULL_FIRST>
struct ObTeeNullSafeDatumStrCmp
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret);
};

} // end namespace ca
} // end namespace common
} // end namespace oceanbase