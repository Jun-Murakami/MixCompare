// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import React from 'react';
import { type SxProps, Slider, Checkbox, FormControl, InputLabel, MenuItem, Select, Box, Typography } from '@mui/material';
// MUI Select の onChange コールバックが受け取るイベントの number 版（型不一致回避のため union で表現）
type SelectNumberEvent =
  | React.ChangeEvent<Omit<HTMLInputElement, 'value'> & { value: number }>
  | (Event & { target: { value: number; name: string } });
import { getSliderState, getToggleState, getComboBoxState } from 'juce-framework-frontend-mirror';
import { useJuceStore } from './useJuceStore';

type SliderProps = {
  identifier: string;
  label?: string;
  orientation?: 'horizontal' | 'vertical';
  sx?: SxProps;
  valueLabelDisplay?: 'auto' | 'on' | 'off';
};

export const JuceBoundSlider: React.FC<SliderProps> = ({ identifier, label, orientation, sx, valueLabelDisplay }) => {
  const sliderState = getSliderState(identifier);
  // 外部ストア（JUCE State）の値/プロパティを購読。Hooks は早期 return より前に無条件で呼ぶ。
  const value = useJuceStore(sliderState?.valueChangedEvent, () => sliderState?.getNormalisedValue() ?? 0);
  const properties = useJuceStore(sliderState?.propertiesChangedEvent, () => sliderState?.properties);

  // ここで安全に早期 return（Hooks 以降なのでルールに抵触しない）
  if (!sliderState) return null;

  const handleChange = (_: Event, nv: number | number[]) => {
    const n = nv as number;
    sliderState.setNormalisedValue(n);
    // setNormalisedValue は backend へ emit するのみでローカルリスナーを発火しない。
    // ドラッグ中の即時反映のためローカルに通知し、外部ストアの値を更新する。
    sliderState.valueChangedEvent.callListeners(undefined);
  };

  const handleMouseDown = () => sliderState.sliderDragStarted();
  const handleCommit = (_: unknown, nv: number | number[]) => {
    const n = nv as number;
    sliderState.setNormalisedValue(n);
    sliderState.sliderDragEnded();
  };

  const scaled = sliderState.getScaledValue();

  return (
    <Box>
      {label || properties?.name ? (
        <Typography variant='caption' sx={{ mb: 0.5, display: 'block' }}>
          {label || properties?.name}: {scaled} {properties?.label}
        </Typography>
      ) : null}
      <Slider
        min={0}
        max={1}
        step={1 / Math.max(1, (properties?.numSteps ?? 2) - 1)}
        value={value}
        onChange={handleChange}
        onMouseDown={handleMouseDown}
        onChangeCommitted={handleCommit}
        orientation={orientation}
        sx={sx}
        valueLabelDisplay={valueLabelDisplay}
      />
    </Box>
  );
};

type ToggleProps = {
  identifier: string;
  label?: string;
};

export const JuceBoundToggle: React.FC<ToggleProps> = ({ identifier, label }) => {
  const toggleState = getToggleState(identifier);
  // 外部ストア（JUCE State）を購読。Hooks は早期 return より前に無条件で呼ぶ。
  const checked = useJuceStore(toggleState?.valueChangedEvent, () => toggleState?.getValue() ?? false);
  const properties = useJuceStore(toggleState?.propertiesChangedEvent, () => toggleState?.properties);

  if (!toggleState) return null;

  const onChange = (_: React.ChangeEvent<HTMLInputElement>, val: boolean) => {
    toggleState.setValue(val);
    // setValue は backend へ emit するのみのため、即時反映用にローカル通知する。
    toggleState.valueChangedEvent.callListeners(undefined);
  };

  return (
    <Box sx={{ display: 'flex', alignItems: 'center', gap: 1 }}>
      <Checkbox checked={checked} onChange={onChange} size='small' />
      <Typography variant='caption'>{label || properties?.name}</Typography>
    </Box>
  );
};

type ComboProps = {
  identifier: string;
  label?: string;
};

export const JuceBoundCombo: React.FC<ComboProps> = ({ identifier, label }) => {
  const comboState = getComboBoxState(identifier);
  // 外部ストア（JUCE State）を購読。Hooks は早期 return より前に無条件で呼ぶ。
  const index = useJuceStore(comboState?.valueChangedEvent, () => comboState?.getChoiceIndex() ?? 0);
  const properties = useJuceStore(comboState?.propertiesChangedEvent, () => comboState?.properties);

  if (!comboState) return null;

  const onChange = (e: SelectNumberEvent) => {
    // union の両辺とも target.value は number なので安全に取り出す
    const i = (e as { target: { value: number } }).target.value;
    comboState.setChoiceIndex(i);
    // setChoiceIndex は backend へ emit するのみのため、即時反映用にローカル通知する。
    comboState.valueChangedEvent.callListeners(undefined);
  };

  const lbl = label || properties?.name || identifier;
  const choices: string[] = properties?.choices || [];

  return (
    <FormControl size='small' fullWidth>
      <InputLabel id={identifier}>{lbl}</InputLabel>
      <Select labelId={identifier} value={index} label={lbl} onChange={onChange}>
        {choices.map((c, i) => (
          <MenuItem value={i} key={i}>
            {c}
          </MenuItem>
        ))}
      </Select>
    </FormControl>
  );
};
