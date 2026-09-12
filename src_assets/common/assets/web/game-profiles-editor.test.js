import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import AppsView from './views/AppsView.vue'

const clientUuid = '12345678-1234-1234-1234-123456789abc'
const settings = {
  DLSS: { value: 'Balanced', index: 3 }, DLSSFrameGen: { value: true },
  Gamma: { value: 1.25 }, FilmGrain: { value: false }, Label: { value: '123' },
}
const settingsFile = '$(HOME)/games/Cyberpunk/UserSettings.json'
const unavailableUuid = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'
const translate = key => ({
  '_common.save': 'Save', '_common.cancel': 'Cancel',
  '_common.do_cmd': 'Do Command', '_common.undo_cmd': 'Undo Command',
  'apps.add_new': 'Add New',
}[key] || key)
let wrapper

async function setup(initialApps = []) {
  let apps = structuredClone(initialApps)
  let rejection = null
  const posts = []
  vi.stubGlobal('fetch', vi.fn(async (url, options = {}) => {
    if (url === './api/apps' && options.method === 'POST') {
      const payload = JSON.parse(options.body)
      posts.push(payload)
      if (rejection) return { ok: false, json: async () => ({ status: false, error: rejection }) }
      const saved = { ...payload, uuid: payload.uuid || 'new-app' }
      apps = [...apps.filter(app => app.uuid !== saved.uuid), saved]
      return { ok: true, json: async () => ({ status: true }) }
    }
    if (url === './api/apps') return { json: async () => ({ apps: structuredClone(apps) }) }
    if (url === './api/config') return { json: async () => ({ platform: 'linux' }) }
    if (url === './api/clients/list') return { json: async () => ({
      status: true, named_certs: [{ uuid: clientUuid, name: 'Moonlight', friendly_name: 'Living room', perm: '0' }],
    }) }
    throw new Error(`Unexpected fetch: ${url}`)
  }))
  wrapper = mount(AppsView, {
    global: { provide: { i18n: { t: translate } }, mocks: { $t: translate } },
  })
  await flushPromises()
  return { posts, reject: message => { rejection = message } }
}

async function click(text) {
  const button = wrapper.findAll('button').find(button => button.text() === text)
  expect(button, `Button ${text}`).toBeTruthy()
  await button.trigger('click')
  await flushPromises()
}

async function edit(name) {
  if (!name) return click('Add New')
  await wrapper.get(`button[aria-label="Edit ${name}"]`).trigger('click')
  await flushPromises()
}

async function addProfile(values = {}) {
  if (!wrapper.get('#appGameProfiles').element.open) await wrapper.get('#appGameProfiles summary').trigger('click')
  await click('+ Add profile')
  const index = wrapper.findAll('[data-game-profile]').length - 1
  for (const [key, value] of Object.entries({ name: 'Default', ...values })) {
    await wrapper.get(`#gameProfile-${index}-${key}`).setValue(value)
  }
}

const field = (key, index = 0) => wrapper.get(`#gameProfile-${index}-${key}`)

afterEach(() => {
  wrapper?.unmount()
  vi.unstubAllGlobals()
})

describe('mounted game settings profile editor', () => {
  it('isolates canceled new drafts, legacy defaults, and existing profile edits', async () => {
    const original = { uuid: 'saved', name: 'Saved', 'game-profiles': [{ name: 'Original', file: '', settings: {} }] }
    const { posts } = await setup([original, { uuid: 'legacy-a', name: 'Legacy A' }, { uuid: 'legacy-b', name: 'Legacy B' }])
    for (const [first, next] of [[null, null], ['Legacy A', 'Legacy B']]) {
      await edit(first)
      await addProfile()
      await click('+ apps.detached_cmds_add')
      await click('Cancel')
      await edit(next)
      expect(wrapper.findAll('[data-game-profile]')).toHaveLength(0)
      expect(wrapper.findAll('.app-editor-command-row')).toHaveLength(0)
      if (!next) await click('Cancel')
    }
    await click('Save')
    expect(posts[0]['game-profiles']).toEqual([])
    await edit('Saved')
    await field('name').setValue('Canceled change')
    await field('file').setValue(settingsFile)
    await field('settings').setValue(JSON.stringify(settings))
    await click('Cancel')
    await edit('Saved')
    expect(field('name').element.value).toBe('Original')
    expect(field('file').element.value).toBe('')
    expect(field('settings').element.value).toBe('{}')
    expect(original['game-profiles'][0]).toEqual({ name: 'Original', file: '', settings: {} })
  })

  it('saves, reopens, removes rows, and preserves fractional FPS without changing Steam metadata or hooks', async () => {
    const steam = {
      uuid: 'steam', name: 'Steam game', source: 'steam', 'steam-appid': '1145360',
      'steam-launch-mode': 'direct', cmd: '', detached: ['setsid steam steam://rungameid/1145360'],
      'prep-cmd': [{ undo: 'setsid steam -shutdown' }],
      'state-cmd': [{ do: 'state-start', undo: 'state-stop', elevated: false }],
      env: { MANGOHUD: '1', LANG: 'en_US.UTF-8' },
      'image-path': '/covers/steam.png', 'working-dir': '/games', 'virtual-display': true,
      metadata: { keep: ['custom'] },
    }
    const { posts } = await setup([steam])
    await edit('Steam game')
    await addProfile({ name: ' Fractional ', width: '1920', height: '1080', fps: '59.940', file: settingsFile, settings: JSON.stringify(settings), client: clientUuid })
    await addProfile({ name: 'No-op' })
    await click('Save')
    const profiles = [
      { name: 'Fractional', file: settingsFile, settings, width: 1920, height: 1080, fps: 59.94, 'client-uuid': clientUuid },
      { name: 'No-op', file: '', settings: {} },
    ]
    expect(posts[0]['game-profiles']).toEqual(profiles)
    expect(posts[0]).toMatchObject(steam)
    await edit('Steam game')
    expect(field('fps').element.value).toBe('59.94')
    expect(field('file').element.value).toBe(settingsFile)
    expect(JSON.parse(field('settings').element.value)).toEqual(settings)
    expect(field('client').element.value).toBe(clientUuid)
    expect(field('client').text()).toContain('Living room')
    await click('Save')
    expect(posts[1]['game-profiles']).toEqual(profiles)
    await edit('Steam game')
    await wrapper.get('[aria-label="Remove game settings profile 1"]').trigger('click')
    await click('Save')
    expect(posts[2]['game-profiles']).toEqual([profiles[1]])
  })

  it('preserves unavailable saved client UUIDs and can explicitly switch to any client', async () => {
    const profile = { name: 'Offline client', file: '', settings: {}, 'client-uuid': unavailableUuid }
    const { posts } = await setup([{ uuid: 'old', name: 'Old', 'game-profiles': [profile] }])
    await edit('Old')
    expect(field('client').element.selectedOptions[0].textContent).toContain(`Unavailable — ${unavailableUuid}`)
    await click('Save')
    expect(posts[0]['game-profiles']).toEqual([profile])
    await edit('Old')
    await field('client').setValue('')
    await click('Save')
    expect(posts[1]['game-profiles'][0]).not.toHaveProperty('client-uuid')
  })

  it('retains syntax and numeric conversion errors accessibly without posting', async () => {
    const { posts } = await setup()
    await click('Add New')
    await addProfile()
    for (const [key, value] of [
      ['settings', '{'], ['settings', ''], ['width', '1920px'], ['width', '0x100'],
      ['width', '1920.0'], ['height', '1e3'], ['fps', '6e1'], ['fps', '60.'],
      ['fps', 'NaN'], ['fps', 'Infinity'], ['fps', '60.000000000000001'],
    ]) {
      const previous = field(key).element.value
      await field(key).setValue(value)
      await click('Save')
      expect(field(key).attributes('aria-invalid')).toBe('true')
      expect(wrapper.get(`#gameProfile-0-${key}-error`).text()).not.toBe('')
      expect(field(key).attributes('aria-describedby')).toContain(`gameProfile-0-${key}-error`)
      expect(field(key).element.value).toBe(value)
      expect(wrapper.get('#appGameProfiles').element.open).toBe(true)
      await field(key).setValue(previous)
    }
    expect(posts).toHaveLength(0)
  })

  it('serializes whole decimal FPS and omits cleared optional inputs', async () => {
    const { posts } = await setup()
    await click('Add New')
    await addProfile({ width: '1920', height: '1080', fps: '60.000' })
    await click('Save')
    expect(posts[0]['game-profiles'][0]).toMatchObject({ width: 1920, height: 1080, fps: 60 })
    await edit('New App')
    for (const key of ['width', 'height', 'fps']) await field(key).setValue('')
    await click('Save')
    expect(posts[1]['game-profiles']).toEqual([{ name: 'Default', file: '', settings: {} }])
  })

  it('leaves schema validation to the server, displays its rejection and retains edits for retry', async () => {
    const api = await setup()
    await click('Add New')
    await addProfile({ width: '1920', height: '1080', fps: '23.976' })
    await addProfile({ settings: '[]' })
    api.reject('game-profiles[1]: settings must be an object')
    await wrapper.get('#appGameProfiles summary').trigger('click')
    await click('Save')
    expect(api.posts).toHaveLength(1)
    expect(wrapper.find('.app-editor-layout').exists()).toBe(true)
    expect(wrapper.get('#appGameProfiles').element.open).toBe(true)
    expect(wrapper.get('p[role="alert"]').text()).toContain('game-profiles[1]: settings must be an object')
    expect(field('fps').element.value).toBe('23.976')
    expect(api.posts[0]['game-profiles'][1].settings).toEqual([])
    expect(field('settings', 1).element.value).toBe('[]')
    await field('settings', 1).setValue('{}')
    api.reject(null)
    await click('Save')
    expect(api.posts[1]['game-profiles'][1]).toEqual({ name: 'Default', file: '', settings: {} })
    expect(wrapper.find('.app-editor-layout').exists()).toBe(false)
  })
})
