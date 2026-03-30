#pragma once

#include <stdint.h>

class CreativeInventory {
public:
  CreativeInventory();

  void open();
  void close();
  bool isOpen() const;

  void cycleHotbarRight();
  void cycleHotbarLeft();

  void moveRight();
  void moveLeft();
  void moveUp();
  void moveDown();
  void pressCross();

  uint8_t heldBlock() const;

  int hotbarSel() const;
  void setHotbarSel(int sel);
  int cursorX() const;
  int cursorY() const;
  int creativePage() const;
  bool usingSlider() const;
  bool cursorHasItem() const;
  uint8_t cursorItem() const;

  uint8_t hotbarAt(int idx) const;
  void setHotbarAt(int idx, uint8_t id);

  static int inventoryItemCount();
  static uint8_t inventoryItemAt(int idx);

private:
  bool m_open;
  int m_hotbarSel;
  int m_cursorX;
  int m_cursorY;
  int m_creativePage;
  bool m_usingSlider;
  bool m_cursorHasItem;
  uint8_t m_cursorItem;
  uint8_t m_hotbar[9];

  int maxPage() const;
};
