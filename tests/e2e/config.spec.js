import { test, expect } from './fixtures/auth.js'
import { randomUUID } from 'node:crypto'

test('config view loads', async ({ loggedInPage }) => {
  const nav = loggedInPage.getByRole('navigation')
  await nav.getByRole('link', { name: /^settings$/i }).click()
  await expect(loggedInPage).toHaveURL(/#\/config/)
  await expect(loggedInPage.getByRole('heading', { name: /^settings$/i })).toBeVisible({ timeout: 10000 })
})

test('apps view loads', async ({ loggedInPage }) => {
  const nav = loggedInPage.getByRole('navigation')
  await nav.getByRole('link', { name: /^library$/i }).click()
  await expect(loggedInPage).toHaveURL(/#\/apps/)
  await expect(loggedInPage.getByRole('heading', { name: /^library$/i })).toBeVisible({ timeout: 10000 })
})

test('game settings profiles save and reopen on a disposable app', async ({ loggedInPage: page }) => {
  test.skip(!process.env.POLARIS_E2E_BASE_URL, 'Supply an explicit disposable POLARIS_E2E_BASE_URL')
  const uuid = randomUUID()
  const name = `E2E game profiles ${uuid}`
  const profile = {
    name: 'Fractional FPS', file: '$(HOME)/games/Cyberpunk/UserSettings.json', width: 1920, height: 1080, fps: 59.94,
    settings: { DLSS: { value: 'Balanced', index: 3 }, DLSSFrameGen: { value: true }, Gamma: { value: 1.25 } },
  }
  const csrf = await page.locator('meta[name="csrf-token"]').getAttribute('content')
  async function mutateApp(path, data) {
    const response = await page.request.post(path, { data, headers: { 'X-CSRF-Token': csrf } })
    expect(response.ok()).toBe(true)
    expect((await response.json()).status).toBe(true)
  }

  try {
    await mutateApp('/api/apps', { uuid, name, cmd: '', 'game-profiles': [] })
    await page.getByRole('navigation').getByRole('link', { name: /^library$/i }).click()
    await page.getByRole('button', { name: `Edit ${name}`, exact: true }).click()
    await page.locator('#appGameProfiles summary').click()
    await page.getByRole('button', { name: '+ Add profile', exact: true }).click()
    for (const [key, value] of Object.entries(profile)) {
      await page.locator(`#gameProfile-0-${key}`).fill(key === 'settings' ? JSON.stringify(value) : String(value))
    }

    await page.getByRole('button', { name: /^save$/i }).first().click()
    await expect(page.locator('.app-editor-layout')).toHaveCount(0)
    const stored = await (await page.request.get('/api/apps')).json()
    expect(stored.apps.find(app => app.uuid === uuid)['game-profiles']).toEqual([profile])
    await page.getByRole('button', { name: `Edit ${name}`, exact: true }).click()
    expect(JSON.parse(await page.getByLabel('Named option edits (JSON)', { exact: true }).inputValue())).toEqual(profile.settings)
    await page.setViewportSize({ width: 390, height: 844 })
    const section = page.locator('#appGameProfiles')
    expect(await section.evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true)
  } finally {
    await mutateApp('/api/apps/delete', { uuid })
    const remaining = await (await page.request.get('/api/apps')).json()
    expect(remaining.apps.some(app => app.uuid === uuid)).toBe(false)
  }
})

test('settings metadata projection answers a session with version 1', async ({ loggedInPage }) => {
  const response = await loggedInPage.request.get('/api/settings/metadata')
  expect(response.status()).toBe(200)
  const body = await response.json()
  expect(body.status).toBe(true)
  expect(body.version).toBe(1)
  expect(Array.isArray(body.modes)).toBe(true)
  expect(typeof body.fields).toBe('object')
  expect(typeof body.stream_display).toBe('object')
  expect(typeof body.auto_quality).toBe('object')

  // The stats channel carries the same tuning and auto-quality blocks at 1 Hz.
  const stats = await loggedInPage.request.get('/api/stats/stream')
  expect(stats.status()).toBe(200)
  const statsBody = await stats.json()
  expect(typeof statsBody.tuning).toBe('object')
  expect(typeof statsBody.auto_quality).toBe('object')
})

test('settings metadata projection requires a web session', async ({ request }) => {
  const response = await request.get('/api/settings/metadata')
  expect([401, 403]).toContain(response.status())
})
