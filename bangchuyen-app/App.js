import React, { useState, useEffect, useRef } from 'react';
import {
  View, Text, ScrollView, Switch, TouchableOpacity,
  StyleSheet, StatusBar, Animated, Dimensions, Platform,
} from 'react-native';
import { db } from './src/config/firebase';
import { ref, onValue, set } from 'firebase/database';

const { width } = Dimensions.get('window');
const FB_ROOT = 'bangchuyen';

// ─── Theme ────────────────────────────────────────────────────
const C = {
  bg:        '#070D1A',
  surface:   '#0F1829',
  card:      '#131E32',
  border:    '#1E2E4A',
  accent:    '#00D4FF',
  green:     '#00FF88',
  red:       '#FF4757',
  orange:    '#FF9F43',
  yellow:    '#FFD32A',
  purple:    '#A55EEA',
  text:      '#E8F4FD',
  subtext:   '#6B8CAE',
  dim:       '#2A3F5F',
};

// ─── Helpers ──────────────────────────────────────────────────
function useFirebaseValue(path, defaultVal) {
  const [val, setVal] = useState(defaultVal);
  useEffect(() => {
    const r = ref(db, path);
    const unsub = onValue(r, snap => {
      const v = snap.val();
      if (v !== null && v !== undefined) setVal(v);
    });
    return () => unsub();
  }, [path]);
  return val;
}

function fbSet(path, value) {
  set(ref(db, path), value);
}

// ─── Connection Badge ─────────────────────────────────────────
function ConnectionBadge({ connected }) {
  const pulse = useRef(new Animated.Value(1)).current;

  useEffect(() => {
    if (!connected) return;
    Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, { toValue: 1.4, duration: 800, useNativeDriver: true }),
        Animated.timing(pulse, { toValue: 1,   duration: 800, useNativeDriver: true }),
      ])
    ).start();
  }, [connected]);

  return (
    <View style={styles.badgeRow}>
      <Animated.View
        style={[
          styles.pulseDot,
          { backgroundColor: connected ? C.green : C.red, transform: [{ scale: pulse }] },
        ]}
      />
      <Text style={[styles.badgeText, { color: connected ? C.green : C.red }]}>
        {connected ? 'ESP32 ONLINE' : 'ESP32 OFFLINE'}
      </Text>
    </View>
  );
}

// ─── Control Toggle Card ──────────────────────────────────────
function ControlCard({ icon, label, sublabel, value, onToggle, color, disabled }) {
  const anim = useRef(new Animated.Value(value ? 1 : 0)).current;

  useEffect(() => {
    Animated.timing(anim, {
      toValue: value ? 1 : 0,
      duration: 300,
      useNativeDriver: false,
    }).start();
  }, [value]);

  const bgColor = anim.interpolate({
    inputRange:  [0, 1],
    outputRange: [C.card, `${color}18`],
  });
  const borderColor = anim.interpolate({
    inputRange:  [0, 1],
    outputRange: [C.border, color],
  });

  return (
    <Animated.View style={[styles.controlCard, { backgroundColor: bgColor, borderColor }]}>
      <View style={styles.controlLeft}>
        <Text style={styles.controlIcon}>{icon}</Text>
        <View>
          <Text style={styles.controlLabel}>{label}</Text>
          <Text style={styles.controlSub}>{sublabel}</Text>
        </View>
      </View>
      <Switch
        value={value}
        onValueChange={disabled ? undefined : onToggle}
        thumbColor={value ? color : C.dim}
        trackColor={{ false: C.border, true: `${color}50` }}
        disabled={disabled}
        style={{ transform: [{ scaleX: 1.1 }, { scaleY: 1.1 }] }}
      />
    </Animated.View>
  );
}

// ─── Mode Toggle (AUTO / MANUAL) ─────────────────────────────
function ModeToggle({ isAuto, onToggle, disabled }) {
  return (
    <View style={styles.modeCard}>
      <Text style={styles.controlIcon}>⚙️</Text>
      <View style={{ flex: 1, marginLeft: 14 }}>
        <Text style={styles.controlLabel}>Chế độ hoạt động</Text>
        <Text style={styles.controlSub}>
          {isAuto ? 'AUTO — AI tự động phân loại' : 'MANUAL — Điều khiển thủ công'}
        </Text>
      </View>
      <TouchableOpacity
        onPress={disabled ? undefined : onToggle}
        activeOpacity={0.8}
        style={[styles.modePill, { backgroundColor: isAuto ? `${C.accent}20` : `${C.orange}20` }]}
      >
        <View style={[styles.modeDot, { backgroundColor: isAuto ? C.accent : C.orange }]} />
        <Text style={[styles.modeText, { color: isAuto ? C.accent : C.orange }]}>
          {isAuto ? 'AUTO' : 'MANUAL'}
        </Text>
      </TouchableOpacity>
    </View>
  );
}

// ─── Stat Card ────────────────────────────────────────────────
function StatCard({ icon, label, value, color }) {
  const scaleAnim = useRef(new Animated.Value(1)).current;
  const prevVal = useRef(value);

  useEffect(() => {
    if (prevVal.current !== value) {
      prevVal.current = value;
      Animated.sequence([
        Animated.timing(scaleAnim, { toValue: 1.15, duration: 120, useNativeDriver: true }),
        Animated.timing(scaleAnim, { toValue: 1,    duration: 200, useNativeDriver: true }),
      ]).start();
    }
  }, [value]);

  return (
    <View style={[styles.statCard, { borderColor: `${color}40` }]}>
      <Text style={styles.statIcon}>{icon}</Text>
      <Animated.Text style={[styles.statValue, { color, transform: [{ scale: scaleAnim }] }]}>
        {value}
      </Animated.Text>
      <Text style={styles.statLabel}>{label}</Text>
    </View>
  );
}

// ─── Last Item Badge ──────────────────────────────────────────
const ITEM_CONFIG = {
  'AP': { label: 'Táo 🍎',    color: C.red    },
  'BA': { label: 'Chuối 🍌',  color: C.yellow },
  'OR': { label: 'Cam 🍊',    color: C.orange },
  'MI': { label: 'Sữa 🥛',    color: C.accent },
  '--': { label: 'Chờ vật…',  color: C.subtext },
};

function LastItemBadge({ item }) {
  const cfg = ITEM_CONFIG[item] || ITEM_CONFIG['--'];
  return (
    <View style={[styles.lastItemCard, { borderColor: `${cfg.color}60`, backgroundColor: `${cfg.color}12` }]}>
      <Text style={styles.lastItemTitle}>Sản phẩm vừa phát hiện</Text>
      <Text style={[styles.lastItemValue, { color: cfg.color }]}>{cfg.label}</Text>
    </View>
  );
}

// ─── Reset Button ─────────────────────────────────────────────
function ResetButton({ onPress, label, color }) {
  const scale = useRef(new Animated.Value(1)).current;
  const handlePress = () => {
    Animated.sequence([
      Animated.timing(scale, { toValue: 0.93, duration: 80, useNativeDriver: true }),
      Animated.timing(scale, { toValue: 1,    duration: 150, useNativeDriver: true }),
    ]).start();
    onPress();
  };
  return (
    <TouchableOpacity onPress={handlePress} activeOpacity={0.8} style={{ flex: 1 }}>
      <Animated.View style={[styles.resetBtn, { borderColor: color, transform: [{ scale }] }]}>
        <Text style={[styles.resetText, { color }]}>{label}</Text>
      </Animated.View>
    </TouchableOpacity>
  );
}

// ─── Main App ─────────────────────────────────────────────────
export default function App() {
  // ── Firebase state ──────────────────────────────────────────
  const systemOn   = useFirebaseValue(`${FB_ROOT}/control/systemOn`,   false);
  const isAutoMode = useFirebaseValue(`${FB_ROOT}/control/isAutoMode`, false);
  const motorOn    = useFirebaseValue(`${FB_ROOT}/control/motorOn`,    false);
  const count1     = useFirebaseValue(`${FB_ROOT}/status/count1`,      0);
  const count2     = useFirebaseValue(`${FB_ROOT}/status/count2`,      0);
  const lastItem   = useFirebaseValue(`${FB_ROOT}/status/lastItem`,    '--');
  const connected  = useFirebaseValue(`${FB_ROOT}/status/connected`,   false);
  const queue      = useFirebaseValue(`${FB_ROOT}/status/queue`,       '');

  // ── Handlers ────────────────────────────────────────────────
  const toggleSystem = () => {
    const next = !systemOn;
    fbSet(`${FB_ROOT}/control/systemOn`, next);
    if (!next) {
      fbSet(`${FB_ROOT}/control/motorOn`, false);
    }
  };
  const toggleAuto  = () => fbSet(`${FB_ROOT}/control/isAutoMode`, !isAutoMode);
  const toggleMotor = () => fbSet(`${FB_ROOT}/control/motorOn`,    !motorOn);
  const resetCount1 = () => fbSet(`${FB_ROOT}/control/reset1`, true);
  const resetCount2 = () => fbSet(`${FB_ROOT}/control/reset2`, true);
  const resetAll    = () => {
    fbSet(`${FB_ROOT}/control/reset1`, true);
    fbSet(`${FB_ROOT}/control/reset2`, true);
    fbSet(`${FB_ROOT}/status/lastItem`, '--');
  };

  return (
    <View style={styles.root}>
      <StatusBar barStyle="light-content" backgroundColor={C.bg} />
      <ScrollView
        contentContainerStyle={styles.scroll}
        showsVerticalScrollIndicator={false}
      >
        {/* ── Header ── */}
        <View style={styles.header}>
          <View>
            <Text style={styles.headerTitle}>🏭 Băng Chuyền</Text>
            <Text style={styles.headerSub}>Hệ Thống Phân Loại Sản Phẩm</Text>
          </View>
          <ConnectionBadge connected={connected} />
        </View>

        {/* ── Section: Điều khiển ── */}
        <Text style={styles.sectionTitle}>⚡ Điều khiển</Text>

        <ControlCard
          icon="🔋"
          label="Hệ thống"
          sublabel={systemOn ? 'Đang hoạt động' : 'Tắt nguồn'}
          value={systemOn}
          onToggle={toggleSystem}
          color={C.green}
        />

        <ModeToggle
          isAuto={isAutoMode}
          onToggle={toggleAuto}
          disabled={!systemOn}
        />

        <ControlCard
          icon="⚙️"
          label="Động cơ băng chuyền"
          sublabel={motorOn ? 'Đang chạy' : 'Đã dừng'}
          value={motorOn}
          onToggle={toggleMotor}
          color={C.orange}
          disabled={!systemOn}
        />

        {/* ── Section: Thống kê ── */}
        <Text style={styles.sectionTitle}>📊 Thống kê phân loại</Text>

        <View style={styles.statsRow}>
          <StatCard icon="🍎🍌🍊" label="Trái cây" value={count1} color={C.green} />
          <StatCard icon="🥛"    label="Sữa"      value={count2} color={C.accent} />
        </View>

        <View style={styles.lastItemCard}>
            <Text style={styles.lastItemTitle}>🕒 Hàng đợi sản phẩm (Max 4)</Text>
            <Text style={[styles.lastItemValue, { color: C.yellow, fontSize: 24 }]}>{queue || "(Trống)"}</Text>
        </View>

        <LastItemBadge item={lastItem} />

        {/* ── Section: Reset ── */}
        <Text style={styles.sectionTitle}>🔄 Đặt lại bộ đếm</Text>
        <View style={styles.resetRow}>
          <ResetButton label="Reset Trái cây" color={C.green}  onPress={resetCount1} />
          <View style={{ width: 10 }} />
          <ResetButton label="Reset Sữa"      color={C.accent} onPress={resetCount2} />
        </View>
        <TouchableOpacity onPress={resetAll} style={styles.resetAllBtn} activeOpacity={0.8}>
          <Text style={styles.resetAllText}>🗑️ Reset tất cả</Text>
        </TouchableOpacity>

        {/* ── Footer ── */}
        <View style={styles.footer}>
          <Text style={styles.footerText}>Hệ Thống Nhúng · ESP32 FreeRTOS v5</Text>
          <Text style={styles.footerText}>Firebase RTDB · Real-time sync</Text>
        </View>
      </ScrollView>
    </View>
  );
}

// ─── Styles ───────────────────────────────────────────────────
const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: C.bg,
  },
  scroll: {
    paddingHorizontal: 18,
    paddingBottom: 40,
    paddingTop: Platform.OS === 'android' ? 50 : 60,
  },

  // Header
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'flex-start',
    marginBottom: 28,
  },
  headerTitle: {
    fontSize: 26,
    fontWeight: '800',
    color: C.text,
    letterSpacing: 0.5,
  },
  headerSub: {
    fontSize: 12,
    color: C.subtext,
    marginTop: 3,
    letterSpacing: 0.3,
  },

  // Badge
  badgeRow: {
    flexDirection: 'row',
    alignItems: 'center',
    backgroundColor: C.surface,
    paddingHorizontal: 10,
    paddingVertical: 6,
    borderRadius: 20,
    borderWidth: 1,
    borderColor: C.border,
    gap: 6,
  },
  pulseDot: {
    width: 8,
    height: 8,
    borderRadius: 4,
  },
  badgeText: {
    fontSize: 10,
    fontWeight: '700',
    letterSpacing: 0.8,
  },

  // Section
  sectionTitle: {
    fontSize: 13,
    fontWeight: '700',
    color: C.subtext,
    letterSpacing: 1.2,
    textTransform: 'uppercase',
    marginBottom: 12,
    marginTop: 4,
  },

  // Control Card
  controlCard: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    padding: 18,
    borderRadius: 16,
    borderWidth: 1.5,
    marginBottom: 12,
  },
  controlLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
    flex: 1,
  },
  controlIcon: {
    fontSize: 26,
  },
  controlLabel: {
    fontSize: 15,
    fontWeight: '700',
    color: C.text,
  },
  controlSub: {
    fontSize: 12,
    color: C.subtext,
    marginTop: 2,
  },

  // Mode Toggle
  modeCard: {
    flexDirection: 'row',
    alignItems: 'center',
    padding: 18,
    borderRadius: 16,
    borderWidth: 1.5,
    borderColor: C.border,
    backgroundColor: C.card,
    marginBottom: 12,
  },
  modePill: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: 12,
    paddingVertical: 8,
    borderRadius: 20,
    gap: 6,
  },
  modeDot: {
    width: 7,
    height: 7,
    borderRadius: 4,
  },
  modeText: {
    fontSize: 12,
    fontWeight: '800',
    letterSpacing: 0.8,
  },

  // Stats
  statsRow: {
    flexDirection: 'row',
    gap: 12,
    marginBottom: 12,
  },
  statCard: {
    flex: 1,
    backgroundColor: C.card,
    borderRadius: 16,
    borderWidth: 1.5,
    alignItems: 'center',
    paddingVertical: 22,
  },
  statIcon: {
    fontSize: 22,
    marginBottom: 8,
  },
  statValue: {
    fontSize: 44,
    fontWeight: '900',
    letterSpacing: -1,
  },
  statLabel: {
    fontSize: 12,
    color: C.subtext,
    marginTop: 4,
    fontWeight: '600',
  },

  // Last Item
  lastItemCard: {
    borderWidth: 1.5,
    borderRadius: 16,
    paddingVertical: 20,
    paddingHorizontal: 20,
    alignItems: 'center',
    marginBottom: 24,
  },
  lastItemTitle: {
    fontSize: 11,
    color: C.subtext,
    letterSpacing: 1,
    textTransform: 'uppercase',
    fontWeight: '600',
    marginBottom: 6,
  },
  lastItemValue: {
    fontSize: 28,
    fontWeight: '800',
    letterSpacing: 0.5,
  },

  // Reset
  resetRow: {
    flexDirection: 'row',
    marginBottom: 10,
  },
  resetBtn: {
    borderWidth: 1.5,
    borderRadius: 14,
    paddingVertical: 14,
    alignItems: 'center',
    backgroundColor: C.card,
  },
  resetText: {
    fontSize: 13,
    fontWeight: '700',
  },
  resetAllBtn: {
    backgroundColor: `${C.red}15`,
    borderWidth: 1.5,
    borderColor: `${C.red}50`,
    borderRadius: 14,
    paddingVertical: 14,
    alignItems: 'center',
    marginBottom: 8,
  },
  resetAllText: {
    fontSize: 13,
    fontWeight: '700',
    color: C.red,
  },

  // Footer
  footer: {
    alignItems: 'center',
    paddingTop: 24,
    gap: 4,
  },
  footerText: {
    fontSize: 11,
    color: C.dim,
    letterSpacing: 0.3,
  },
});
