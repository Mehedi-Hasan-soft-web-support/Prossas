-- Proshash Dynamic QR Supabase Schema
-- Paste this into Supabase Dashboard > SQL Editor > New query > Run.
-- Prototype policy: anon can read/insert/update. This is easy for testing, not production-safe.

create extension if not exists pgcrypto;

create table if not exists public.patients (
  id uuid primary key default gen_random_uuid(),
  patient_code text not null unique,
  name text not null,
  age integer,
  phone text,
  gender text,
  address text,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.therapy_sessions (
  id uuid primary key default gen_random_uuid(),
  session_code text not null unique default ('S-' || upper(substr(gen_random_uuid()::text, 1, 8))),

  patient_id uuid references public.patients(id) on delete set null,
  patient_code text,
  patient_name text,
  patient_phone text,

  medicine_name text,
  dose text,
  doctor_note text,

  device_id text not null,
  device_name text default 'Proshash Nebulizer',
  qr_token text,
  qr_payload text,

  requested_duration_min integer,
  selected_duration_min integer,
  actual_duration_sec integer,

  status text not null default 'assigned' check (status in ('assigned', 'running', 'completed', 'cancelled', 'offline')),

  assigned_at timestamptz not null default now(),
  qr_scanned_at timestamptz not null default now(),
  started_at timestamptz,
  completed_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

-- Add columns if you already ran an older Proshash schema.
alter table public.therapy_sessions add column if not exists device_name text default 'Proshash Nebulizer';
alter table public.therapy_sessions add column if not exists qr_token text;
alter table public.therapy_sessions add column if not exists qr_payload text;
alter table public.therapy_sessions add column if not exists requested_duration_min integer;

create index if not exists idx_patients_code on public.patients(patient_code);
create index if not exists idx_patients_phone on public.patients(phone);
create index if not exists idx_sessions_device_token_status on public.therapy_sessions(device_id, qr_token, status);
create index if not exists idx_sessions_patient_code on public.therapy_sessions(patient_code);
create index if not exists idx_sessions_created_at on public.therapy_sessions(created_at desc);
create unique index if not exists idx_sessions_unique_qr_token on public.therapy_sessions(qr_token) where qr_token is not null;

create or replace function public.set_updated_at()
returns trigger as $$
begin
  new.updated_at = now();
  return new;
end;
$$ language plpgsql;

drop trigger if exists set_patients_updated_at on public.patients;
create trigger set_patients_updated_at
before update on public.patients
for each row execute function public.set_updated_at();

drop trigger if exists set_sessions_updated_at on public.therapy_sessions;
create trigger set_sessions_updated_at
before update on public.therapy_sessions
for each row execute function public.set_updated_at();

alter table public.patients enable row level security;
alter table public.therapy_sessions enable row level security;

-- Reset prototype policies so this SQL can be run multiple times.
drop policy if exists "prototype anon read patients" on public.patients;
drop policy if exists "prototype anon insert patients" on public.patients;
drop policy if exists "prototype anon update patients" on public.patients;
drop policy if exists "prototype anon delete patients" on public.patients;

drop policy if exists "prototype anon read sessions" on public.therapy_sessions;
drop policy if exists "prototype anon insert sessions" on public.therapy_sessions;
drop policy if exists "prototype anon update sessions" on public.therapy_sessions;
drop policy if exists "prototype anon delete sessions" on public.therapy_sessions;

create policy "prototype anon read patients"
on public.patients for select to anon using (true);

create policy "prototype anon insert patients"
on public.patients for insert to anon with check (true);

create policy "prototype anon update patients"
on public.patients for update to anon using (true) with check (true);

create policy "prototype anon delete patients"
on public.patients for delete to anon using (true);

create policy "prototype anon read sessions"
on public.therapy_sessions for select to anon using (true);

create policy "prototype anon insert sessions"
on public.therapy_sessions for insert to anon with check (true);

create policy "prototype anon update sessions"
on public.therapy_sessions for update to anon using (true) with check (true);

create policy "prototype anon delete sessions"
on public.therapy_sessions for delete to anon using (true);

grant usage on schema public to anon, authenticated;
grant select, insert, update, delete on public.patients to anon, authenticated;
grant select, insert, update, delete on public.therapy_sessions to anon, authenticated;
