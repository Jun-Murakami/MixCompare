// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import React from 'react';
import {
  Button,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
  TextField,
} from '@mui/material';

import { useDialogStore, type DialogOptions } from '../store/dialogStore';

export const GlobalDialog: React.FC = () => {
  const { isOpen, options, handleClose, dialogId } = useDialogStore();

  if (!options) return null;

  // 入力状態は DialogBody が保持する。ダイアログを開くたびに変わる dialogId を
  // key に渡すことで再マウントし、defaultValue を初期値として反映する
  // （options 変化を useEffect で監視して setState する必要がない）。
  return (
    <DialogBody key={dialogId} options={options} isOpen={isOpen} handleClose={handleClose} />
  );
};

interface DialogBodyProps {
  options: DialogOptions;
  isOpen: boolean;
  handleClose: (result: string | null) => void;
}

const DialogBody: React.FC<DialogBodyProps> = ({ options, isOpen, handleClose }) => {
  // 入力モード用のローカル状態。key による再マウントで defaultValue が初期反映される。
  const [inputValue, setInputValue] = React.useState(options.input?.defaultValue ?? '');
  const [inputError, setInputError] = React.useState<string | null>(null);

  // 入力モードかどうか
  const isInputMode = Boolean(options.input);

  // 即時バリデーション関数
  const validateNow = (value: string): string | null => {
    // 必須チェック
    if (options.input?.required && value.trim().length === 0) {
      return '入力は必須です';
    }
    // カスタムバリデーション
    if (options.input?.validate) {
      try {
        return options.input.validate(value);
      } catch {
        // バリデータ内例外はアプリの中央ハンドラ（Sentry等）に委譲、ここではユーザー向け一般メッセージ
        return '無効な入力です';
      }
    }
    return null;
  };

  // プライマリボタン確定
  const handlePrimaryClick = () => {
    if (isInputMode) {
      const err = validateNow(inputValue);
      setInputError(err);
      if (err) return; // 無効時は閉じない
      // 入力モードでは入力値を結果として返す
      handleClose(inputValue);
      return;
    }
    // 通常モードは押下ボタンのテキストを返す
    const primaryText = options.primaryButton?.text ?? 'OK';
    handleClose(primaryText);
  };

  // セカンダリ/ターシャリは常にボタンラベルで閉じる
  const handleSecondaryClick = () => {
    if (options.secondaryButton) {
      handleClose(options.secondaryButton.text);
    }
  };
  const handleTertiaryClick = () => {
    if (options.tertiaryButton) {
      handleClose(options.tertiaryButton.text);
    }
  };

  // 入力中 Enter で確定
  const handleInputKeyDown: React.KeyboardEventHandler<HTMLInputElement> = (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      handlePrimaryClick();
    }
  };

  // 本文は string の場合のみ pre-line で折り返し
  const renderContent = () => {
    if (typeof options.content === 'string') {
      return <DialogContentText sx={{ whiteSpace: 'pre-line', fontSize: '0.85rem' }}>{options.content}</DialogContentText>;
    }
    return options.content;
  };

  const primaryButton = options.primaryButton || { text: 'OK', color: 'primary', variant: 'contained' };
  const hasCustomButtons = options.primaryButton || options.secondaryButton || options.tertiaryButton;

  // 入力モード時のプライマリボタン無効条件
  // - 必須指定かつ未入力（trim後に空）の場合は無効
  // - カスタムvalidateは確定時にのみ評価するため、ここでは考慮しない
  const isPrimaryDisabled = isInputMode
    ? Boolean(options.input?.required) && inputValue.trim().length === 0
    : false;

  return (
    <Dialog
      open={isOpen}
      onClose={() => handleClose(null)}
      maxWidth="sm"
      fullWidth
      closeAfterTransition
      disableRestoreFocus
    >
      <DialogTitle sx={{ fontSize: '0.9rem', fontWeight: 600 }}>{options.title}</DialogTitle>
      <DialogContent sx={{ fontSize: '0.85rem' }}>
        {renderContent()}
        {isInputMode && (
          <TextField
            autoFocus
            margin="dense"
            fullWidth
            label={options.input?.label}
            placeholder={options.input?.placeholder}
            value={inputValue}
            type={options.input?.type ?? 'text'}
            onChange={(e) => {
              const value = e.target.value;
              setInputValue(value);
              // 入力中はバリデーションしない（エラー表示のクリアのみ）
              setInputError(null);
            }}
            onKeyDown={handleInputKeyDown}
            error={Boolean(inputError)}
            helperText={inputError ?? options.input?.helperText}
            slotProps={{
              input: {
                inputProps: { 'data-testid': 'global-dialog-input' },
              },
            }}
          />
        )}
      </DialogContent>
      <DialogActions>
        {hasCustomButtons ? (
          <>
            {options.primaryButton && (
              <Button
                onClick={handlePrimaryClick}
                color={options.primaryButton.color || 'primary'}
                variant={options.primaryButton.variant || 'contained'}
                disabled={isPrimaryDisabled}
                size='small'
              >
                {options.primaryButton.text}
              </Button>
            )}
            {options.secondaryButton && (
              <Button
                onClick={handleSecondaryClick}
                color={options.secondaryButton.color || 'inherit'}
                variant={options.secondaryButton.variant || 'text'}
                size='small'
              >
                {options.secondaryButton.text}
              </Button>
            )}
            {options.tertiaryButton && (
              <Button
                onClick={handleTertiaryClick}
                color={options.tertiaryButton.color || 'inherit'}
                variant={options.tertiaryButton.variant || 'text'}
                size='small'
              >
                {options.tertiaryButton.text}
              </Button>
            )}
          </>
        ) : (
          <Button
            onClick={handlePrimaryClick}
            color={primaryButton.color}
            variant={primaryButton.variant}
            disabled={isPrimaryDisabled}
            size='small'
          >
            {primaryButton.text}
          </Button>
        )}
      </DialogActions>
    </Dialog>
  );
};