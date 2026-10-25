// SPDX-License-Identifier: BSD-2-Clause-Patent
// Keep the selected logical entry in the bounded visible row window.
#ifndef UNIXNU_MENU_VIEWPORT_H
#define UNIXNU_MENU_VIEWPORT_H
STATIC UINTN UniXNUMenuFirstVisible(UINTN Count,UINTN Rows,UINTN Selected,UINTN First) {
  if (Count == 0 || Rows == 0) { return 0; }
  if (Rows > Count) { Rows = Count; }
  if (Selected >= Count) { Selected = Count - 1; }
  if (First > Count - Rows) { First = Count - Rows; }
  if (Selected < First) { return Selected; }
  if (Selected - First >= Rows) { return Selected - Rows + 1; }
  return First;
}
#endif
